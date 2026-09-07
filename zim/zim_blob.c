/*
 * ZIM cluster decompression shared by the host diagnostics and WikiReader.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zim_blob.h"

#include <stdlib.h>
#include <string.h>

#if defined(__c33__)
void *zim_alloc_bank_local(size_t size);
void *zim_alloc_other_bank(size_t size, const void *avoid);
#else
#define zim_alloc_bank_local malloc
#define zim_alloc_other_bank(size, avoid) malloc(size)
#endif

#define ZIM_REDIRECT_MIME 0xffff
#define ZIM_REDIRECT_LIMIT 64
#define ZIM_STREAM_BUFFER_SIZE (64 * 1024)
#define ZIM_OFFSET_TABLE_LIMIT (4 * 1024 * 1024)
/* Kiwix closes a cluster once it holds 2 MiB, so one blob may push a cluster
 * somewhat past that.  Sizing the first attempt above the common case avoids
 * a second decode of the leading blocks for nearly every cluster. */
#ifndef ZIM_CLUSTER_GUESS_SIZE
#define ZIM_CLUSTER_GUESS_SIZE ((2u << 20) + (512u << 10))
#endif
/* Larger clusters are decoded through a 64 KiB window instead of being kept. */
#ifndef ZIM_CLUSTER_LIMIT
#define ZIM_CLUSTER_LIMIT (8u << 20)
#endif
#define ZSTD_CONTENTSIZE_UNKNOWN (~0ULL)
#define ZSTD_CONTENTSIZE_ERROR (~1ULL)
/* ZSTD_d_stableOutBuffer in the pinned vendored decoder. */
#define ZSTD_D_STABLE_OUT_BUFFER 1001

/* The vendored decoder is deliberately headerless. Keep the small public
 * streaming interface here so host and freestanding builds use the same code. */
typedef struct ZSTD_DCtx_s ZSTD_DStream;
typedef struct {
	const void *src;
	size_t size;
	size_t pos;
} ZSTD_inBuffer;
typedef struct {
	void *dst;
	size_t size;
	size_t pos;
} ZSTD_outBuffer;

typedef struct {
	void *(*customAlloc)(void *opaque, size_t size);
	void (*customFree)(void *opaque, void *address);
	void *opaque;
} ZSTD_customMem;
extern ZSTD_DStream *ZSTD_createDStream(void);
extern ZSTD_DStream *ZSTD_createDStream_advanced(ZSTD_customMem memory);
extern size_t ZSTD_DCtx_literalBufferSize(void);
extern size_t ZSTD_DCtx_setLiteralBuffer(ZSTD_DStream *stream, void *buffer);
#if defined(__c33__)
extern size_t ZSTD_DCtx_hufTableSize(void);
extern size_t ZSTD_DCtx_setHufTableBuffer(ZSTD_DStream *stream, void *buffer);
#endif
extern size_t ZSTD_freeDStream(ZSTD_DStream *stream);
extern size_t ZSTD_initDStream(ZSTD_DStream *stream);
extern size_t ZSTD_decompressStream(ZSTD_DStream *stream,
				    ZSTD_outBuffer *output,
				    ZSTD_inBuffer *input);
extern unsigned long long ZSTD_getFrameContentSize(const void *source,
						   size_t source_size);
extern size_t ZSTD_DCtx_setParameter(ZSTD_DStream *stream, int parameter,
				     int value);
extern unsigned ZSTD_isError(size_t code);

/*
 * The most recently touched Zstandard cluster stays decoded in memory together
 * with its live decoder.  Articles are stored path-ordered, so a cluster holds
 * dozens of neighbouring pages; history navigation and many link follows land
 * in the cluster that was just decoded.  Blobs below the decoded frontier are
 * a memcpy, and blobs beyond it continue the existing stream rather than
 * starting the frame again.
 *
 * Zstandard's stable output mode writes directly into this buffer, which must
 * therefore be large enough for the whole frame.  Kiwix omits the frame
 * content size, so the buffer is first sized by a generous guess and re-sized
 * exactly once if the leading offset table reports a larger cluster.
 */
/* The literal scratch buffer and the literal Huffman table do not depend
 * on the cluster, so they are placed once and kept.  Allocating them per
 * cluster meant a bank walk per article, whose cost depends on the state
 * of the heap: on one device run it took 200 ms that no timer accounted
 * for, while the emulator's heap needed no fillers at all. */
static unsigned char *scratch_literals;
#if defined(__c33__)
static unsigned char *scratch_huftable;
#endif

typedef struct {
	int valid;
	const void *archive_id;
	uint64_t cluster_start;
	uint64_t cluster_end;
	uint64_t compressed_pos;
	ZSTD_DStream *stream;
	unsigned char *input;
	ZSTD_inBuffer in;
	unsigned char *output;
	size_t capacity;
	size_t decoded;
	size_t table_size;
	size_t total;
	size_t offset_size;
	int finished;
} ZIM_CLUSTER;

static ZIM_CLUSTER cluster;

static uint32_t get_le32(const unsigned char *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
		(uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t get_le64(const unsigned char *p)
{
	return (uint64_t)get_le32(p) | (uint64_t)get_le32(p + 4) << 32;
}

static uint64_t get_offset(const unsigned char *p, size_t offset_size)
{
	return offset_size == 8 ? get_le64(p) : get_le32(p);
}

static int read_exact(const ZIM_ARCHIVE *archive, uint64_t offset,
		      void *buffer, size_t length)
{
	int rc;

	if (offset > archive->io.size ||
	    (uint64_t)length > archive->io.size - offset)
		return ZIM_ERR_RANGE;
	rc = archive->io.read_at(archive->io.opaque, offset, buffer, length);
	return rc ? ZIM_ERR_IO : ZIM_OK;
}

int zim_archive_resolve_redirect(const ZIM_ARCHIVE *archive,
				 ZIM_DIRENT *dirent)
{
	unsigned int redirects = 0;
	int rc;

	if (!archive || !dirent)
		return ZIM_ERR_RANGE;
	while (dirent->mime_type == ZIM_REDIRECT_MIME) {
		if (++redirects > ZIM_REDIRECT_LIMIT ||
		    dirent->redirect_index >= archive->entry_count)
			return ZIM_ERR_FORMAT;
		rc = zim_archive_read_dirent(archive, dirent->redirect_index,
					     dirent);
		if (rc && rc != ZIM_ERR_TRUNCATED)
			return rc;
	}
	return ZIM_OK;
}

static void copy_output_range(unsigned char *destination, size_t capacity,
			      uint64_t blob_start, uint64_t blob_end,
			      uint64_t output_offset,
			      const unsigned char *output, size_t output_size)
{
	uint64_t chunk_end = output_offset + output_size;
	uint64_t copy_start;
	uint64_t copy_end;
	size_t destination_offset;
	size_t amount;

	if (chunk_end <= blob_start || output_offset >= blob_end)
		return;
	copy_start = output_offset > blob_start ? output_offset : blob_start;
	copy_end = chunk_end < blob_end ? chunk_end : blob_end;
	destination_offset = (size_t)(copy_start - blob_start);
	if (destination_offset >= capacity)
		return;
	amount = (size_t)(copy_end - copy_start);
	if (amount > capacity - destination_offset)
		amount = capacity - destination_offset;
	if (amount)
		memcpy(destination + destination_offset,
		       output + (size_t)(copy_start - output_offset), amount);
}

static int read_uncompressed_blob(const ZIM_ARCHIVE *archive,
				  const ZIM_DIRENT *dirent,
				  uint64_t cluster_start,
				  uint64_t cluster_end,
				  uint8_t extended,
				  void *buffer, size_t capacity,
				  size_t *blob_size,
				  ZIM_BLOB_PROGRESS progress,
				  void *progress_opaque)
{
	unsigned char offsets[16];
	uint64_t offset_size = extended ? 8 : 4;
	uint64_t table_pos = cluster_start + 1 +
		(uint64_t)dirent->blob_number * offset_size;
	uint64_t start;
	uint64_t end;
	uint64_t length;
	size_t amount;
	int rc;

	if (table_pos > cluster_end || offset_size * 2 > cluster_end - table_pos)
		return ZIM_ERR_FORMAT;
	rc = read_exact(archive, table_pos, offsets, (size_t)offset_size * 2);
	if (rc)
		return rc;
	start = get_offset(offsets, (size_t)offset_size);
	end = get_offset(offsets + offset_size, (size_t)offset_size);
	if (end < start || start > cluster_end - cluster_start - 1 ||
	    end > cluster_end - cluster_start - 1)
		return ZIM_ERR_FORMAT;
	length = end - start;
	if (length > (uint64_t)(size_t)-1)
		return ZIM_ERR_RANGE;
	*blob_size = (size_t)length;
	amount = *blob_size < capacity ? *blob_size : capacity;
	if (progress)
		progress(progress_opaque, 0, length);
	if (amount) {
		rc = read_exact(archive, cluster_start + 1 + start,
				buffer, amount);
		if (rc)
			return rc;
	}
	if (progress)
		progress(progress_opaque, amount, length);
	return capacity < *blob_size ? ZIM_ERR_TRUNCATED : ZIM_OK;
}

/* SDRAM bank placement of the decoder's streams: the controller keeps one
 * row open per bank, so buffers used together belong in different banks.
 * The placement is relative rather than by bank number, because the stride
 * differs between boards and the register does not give it (4 MB on both
 * units measured, including the 32 MB board whose ADDRC implies 8 MB; see
 * zim_alloc.c).  The cluster output with its literal scratch buffer goes
 * away from the text buffer the converter writes, and the decoder context
 * away from the output. */
static const void *decoder_avoid;

/* The buffer the converter writes while reading the decoded cluster; the
 * reader registers it so the output can be placed in another bank. */
static const void *reader_buffer;

void zim_blob_set_reader_buffer(const void *buffer)
{
	reader_buffer = buffer;
}

static void *decoder_alloc(void *opaque, size_t size)
{
	(void)opaque;
	return zim_alloc_other_bank(size, decoder_avoid);
}

static void decoder_free(void *opaque, void *address)
{
	(void)opaque;
	free(address);
}

static const ZSTD_customMem decoder_memory = { decoder_alloc, decoder_free, NULL };

static void cluster_release(void)
{
	ZSTD_freeDStream(cluster.stream);
	free(cluster.output);
	free(cluster.input);
	memset(&cluster, 0, sizeof(cluster));
}

void zim_blob_cache_reset(void)
{
	cluster_release();
}

/* Compressed bytes handed to the decoder per call.  Each slice is one card
 * read and one progress report.  The card charges about 1.2 ms per read
 * command before the first byte (measured 2026-09-05), so 4 KiB slices
 * spent a fifth of the read time on latency; 16 KiB slices keep the bar
 * moving every quarter second of decoding or so.  The slice is also how
 * far past the blob's end the decoder runs, since each call decodes all
 * the blocks its input covers (the stable output buffer rules out
 * bounding the output instead): 64 KiB slices decoded 100 ms of Cat's
 * cluster that nothing read.  The streaming decoder copies a block that
 * straddles a slice end into its own buffer first, about 4 ms of Cat. */
#define ZIM_CLUSTER_INPUT_SLICE (16u << 10)
#if ZIM_CLUSTER_INPUT_SLICE > ZIM_STREAM_BUFFER_SIZE
#error "the input slice must fit the stream buffer"
#endif

static int cluster_fill_input(const ZIM_ARCHIVE *archive)
{
	size_t amount;
	int rc;

	/* The frame must end inside its cluster. */
	if (cluster.compressed_pos >= cluster.cluster_end)
		return ZIM_ERR_FORMAT;
	amount = ZIM_CLUSTER_INPUT_SLICE;
	if ((uint64_t)amount > cluster.cluster_end - cluster.compressed_pos)
		amount = (size_t)(cluster.cluster_end - cluster.compressed_pos);
	rc = read_exact(archive, cluster.compressed_pos, cluster.input, amount);
	if (rc)
		return rc;
	cluster.compressed_pos += amount;
	cluster.in.src = cluster.input;
	cluster.in.size = amount;
	cluster.in.pos = 0;
	return ZIM_OK;
}

/* Decode until at least `target` bytes of the cluster are present. */
static int cluster_advance(const ZIM_ARCHIVE *archive, size_t target,
			   ZIM_BLOB_PROGRESS progress, void *progress_opaque,
			   uint64_t report_total)
{
	while (cluster.decoded < target && !cluster.finished) {
		ZSTD_outBuffer out;
		size_t remaining;
		int rc;

		if (cluster.in.pos == cluster.in.size) {
			rc = cluster_fill_input(archive);
			if (rc)
				return rc;
		}
		out.dst = cluster.output;
		out.size = cluster.capacity;
		out.pos = cluster.decoded;
		remaining = ZSTD_decompressStream(cluster.stream, &out,
						  &cluster.in);
		if (ZSTD_isError(remaining)) {
			return ZIM_ERR_FORMAT;
		}
		cluster.decoded = out.pos;
		if (!remaining)
			cluster.finished = 1;
		if (progress)
			progress(progress_opaque,
				 cluster.decoded < report_total ?
				 cluster.decoded : report_total, report_total);
	}
	return cluster.decoded >= target ? ZIM_OK : ZIM_ERR_FORMAT;
}

/* Start decoding a cluster.  A zero capacity sizes the output from the frame
 * header when present and from ZIM_CLUSTER_GUESS_SIZE otherwise. */
static int cluster_open(const ZIM_ARCHIVE *archive, uint64_t cluster_start,
			uint64_t cluster_end, uint8_t extended,
			size_t capacity)
{
	unsigned long long content_size;
	int rc;

	cluster_release();
	cluster.input = malloc(ZIM_STREAM_BUFFER_SIZE);
	if (!cluster.input) {
		cluster_release();
		return ZIM_ERR_IO;
	}
	cluster.archive_id = archive->io.opaque;
	cluster.cluster_start = cluster_start;
	cluster.cluster_end = cluster_end;
	cluster.compressed_pos = cluster_start + 1;
	cluster.offset_size = extended ? 8 : 4;
	rc = cluster_fill_input(archive);
	if (rc) {
		cluster_release();
		return rc;
	}
	if (!capacity) {
		content_size = ZSTD_getFrameContentSize(cluster.input,
							cluster.in.size);
		if (content_size == ZSTD_CONTENTSIZE_UNKNOWN ||
		    content_size == ZSTD_CONTENTSIZE_ERROR)
			capacity = ZIM_CLUSTER_GUESS_SIZE;
		else if (content_size <= ZIM_CLUSTER_LIMIT)
			capacity = (size_t)content_size;
		else {
			cluster_release();
			return ZIM_ERR_RANGE;
		}
	}
	/* The output away from the text buffer, the context away from the
	 * output (see decoder_avoid), and the literal scratch buffer away
	 * from both the output and the compressed input: the literal decoder
	 * reads the input and writes the literals, and the sequence loop
	 * reads the literals and writes the output, so with any two in one
	 * bank each copy would open two rows. */
	cluster.output = zim_alloc_other_bank(capacity, reader_buffer);
	decoder_avoid = cluster.output;
	cluster.stream = ZSTD_createDStream_advanced(decoder_memory);
	if (!scratch_literals)
		scratch_literals = zim_alloc_other_bank(ZSTD_DCtx_literalBufferSize(),
							reader_buffer);
#if defined(__c33__)
	/* The literal Huffman table in a bank other than the literals', which
	 * the literal decoder writes while reading it. */
	if (!scratch_huftable)
		scratch_huftable = zim_alloc_other_bank(ZSTD_DCtx_hufTableSize(),
							scratch_literals);
	if (!scratch_huftable ||
	    ZSTD_isError(ZSTD_DCtx_setHufTableBuffer(cluster.stream,
						     scratch_huftable))) {
		cluster_release();
		return ZIM_ERR_IO;
	}
#endif
	if (!cluster.output || !cluster.stream || !scratch_literals ||
	    ZSTD_isError(ZSTD_DCtx_setLiteralBuffer(cluster.stream,
						    scratch_literals)) ||
	    ZSTD_isError(ZSTD_DCtx_setParameter(cluster.stream,
						ZSTD_D_STABLE_OUT_BUFFER, 1)) ||
	    ZSTD_isError(ZSTD_initDStream(cluster.stream))) {
		cluster_release();
		return ZIM_ERR_IO;
	}
	cluster.capacity = capacity;
	cluster.valid = 1;
	return ZIM_OK;
}

/* Decode the leading blob offset table.  ZIM_ERR_RANGE with cluster.total set
 * means the cluster is larger than the current output buffer. */
static int cluster_read_table(const ZIM_ARCHIVE *archive)
{
	uint64_t first_offset;
	uint64_t total;
	int rc;

	if (cluster.total)
		return cluster.total > cluster.capacity ? ZIM_ERR_RANGE : ZIM_OK;
	rc = cluster_advance(archive, cluster.offset_size, NULL, NULL, 0);
	if (rc)
		return rc;
	first_offset = get_offset(cluster.output, cluster.offset_size);
	if (first_offset < 2 * cluster.offset_size ||
	    first_offset % cluster.offset_size ||
	    first_offset > ZIM_OFFSET_TABLE_LIMIT ||
	    first_offset > cluster.capacity)
		return ZIM_ERR_FORMAT;
	rc = cluster_advance(archive, (size_t)first_offset, NULL, NULL, 0);
	if (rc)
		return rc;
	total = get_offset(cluster.output + (size_t)first_offset -
			   cluster.offset_size, cluster.offset_size);
	if (total < first_offset || total > (uint64_t)(size_t)-1)
		return ZIM_ERR_FORMAT;
	cluster.table_size = (size_t)first_offset;
	cluster.total = (size_t)total;
	return cluster.total > cluster.capacity ? ZIM_ERR_RANGE : ZIM_OK;
}

static int cluster_matches(const ZIM_ARCHIVE *archive, uint64_t cluster_start,
			   uint64_t cluster_end)
{
	return cluster.valid && cluster.archive_id == archive->io.opaque &&
		cluster.cluster_start == cluster_start &&
		cluster.cluster_end == cluster_end;
}

/* Fallback for clusters that cannot be held whole: decode through a 64 KiB
 * window and keep only the requested range. */
static int read_zstd_blob_streaming(const ZIM_ARCHIVE *archive,
				    const ZIM_DIRENT *dirent,
				    uint64_t cluster_start, uint64_t cluster_end,
				    uint8_t extended,
				    void *buffer, size_t capacity,
				    size_t *blob_size,
				    ZIM_BLOB_PROGRESS progress,
				    void *progress_opaque)
{
	unsigned char *input_buffer = NULL;
	unsigned char *output_buffer = NULL;
	unsigned char *offset_table = NULL;
	ZSTD_DStream *stream = NULL;
	ZSTD_inBuffer input = { NULL, 0, 0 };
	ZSTD_outBuffer output;
	uint64_t compressed_pos = cluster_start + 1;
	uint64_t output_offset = 0;
	uint64_t blob_start = 0;
	uint64_t blob_end = 0;
	uint64_t table_need64;
	size_t offset_size = extended ? 8 : 4;
	size_t table_need;
	size_t table_have = 0;
	int range_known = 0;
	int result = ZIM_ERR_FORMAT;

	table_need64 = ((uint64_t)dirent->blob_number + 2) * offset_size;
	if (table_need64 > ZIM_OFFSET_TABLE_LIMIT)
		return ZIM_ERR_RANGE;
	table_need = (size_t)table_need64;
	input_buffer = malloc(ZIM_STREAM_BUFFER_SIZE);
	output_buffer = malloc(ZIM_STREAM_BUFFER_SIZE);
	offset_table = malloc(table_need);
	stream = ZSTD_createDStream();
	if (!input_buffer || !output_buffer || !offset_table || !stream) {
		result = ZIM_ERR_IO;
		goto out;
	}
	if (ZSTD_isError(ZSTD_initDStream(stream)))
		goto out;
	output.dst = output_buffer;
	output.size = ZIM_STREAM_BUFFER_SIZE;

	for (;;) {
		size_t remaining;

		if (input.pos == input.size) {
			size_t amount;
			if (compressed_pos >= cluster_end)
				goto out;
			amount = (cluster_end - compressed_pos > ZIM_STREAM_BUFFER_SIZE) ?
				ZIM_STREAM_BUFFER_SIZE :
				(size_t)(cluster_end - compressed_pos);
			result = read_exact(archive, compressed_pos,
					    input_buffer, amount);
			if (result)
				goto out;
			result = ZIM_ERR_FORMAT;
			compressed_pos += amount;
			input.src = input_buffer;
			input.size = amount;
			input.pos = 0;
		}
		output.pos = 0;
		remaining = ZSTD_decompressStream(stream, &output, &input);
		if (ZSTD_isError(remaining))
			goto out;

		if (!range_known) {
			size_t amount = output.pos;
			if (amount > table_need - table_have)
				amount = table_need - table_have;
			memcpy(offset_table + table_have, output_buffer, amount);
			table_have += amount;
			if (table_have == table_need) {
				uint64_t first_offset =
					get_offset(offset_table, offset_size);
				blob_start = get_offset(offset_table +
					(size_t)dirent->blob_number * offset_size,
					offset_size);
				blob_end = get_offset(offset_table +
					((size_t)dirent->blob_number + 1) * offset_size,
					offset_size);
				if (first_offset < table_need ||
				    first_offset % offset_size ||
				    blob_start < first_offset || blob_end < blob_start)
					goto out;
				if (blob_end - blob_start > (uint64_t)(size_t)-1) {
					result = ZIM_ERR_RANGE;
					goto out;
				}
				*blob_size = (size_t)(blob_end - blob_start);
				range_known = 1;
			}
		}
		if (range_known) {
			uint64_t completed = output_offset + output.pos;

			copy_output_range(buffer, capacity, blob_start, blob_end,
					  output_offset, output_buffer, output.pos);
			if (completed > blob_end)
				completed = blob_end;
			if (progress)
				progress(progress_opaque, completed, blob_end);
			if (completed >= blob_end) {
				result = capacity < *blob_size ?
					ZIM_ERR_TRUNCATED : ZIM_OK;
				goto out;
			}
		}
		output_offset += output.pos;
		if (!remaining)
			goto out;
	}

out:
	ZSTD_freeDStream(stream);
	free(offset_table);
	free(output_buffer);
	free(input_buffer);
	return result;
}

/* Make one blob of a Zstandard cluster available in the cache.  On success
 * the blob occupies cluster.output[*blob_start, *blob_end).  On any failure
 * the cache is released so the caller can fall back to windowed decoding. */
static int cached_blob(const ZIM_ARCHIVE *archive,
		       const ZIM_DIRENT *dirent,
		       uint64_t cluster_start, uint64_t cluster_end,
		       uint8_t extended,
		       size_t *blob_start, size_t *blob_end,
		       ZIM_BLOB_PROGRESS progress,
		       void *progress_opaque)
{
	const unsigned char *table;
	uint64_t start;
	uint64_t end;
	int rc;

	if (!cluster_matches(archive, cluster_start, cluster_end)) {
		rc = cluster_open(archive, cluster_start, cluster_end, extended,
				  0);
		if (rc)
			goto fail;
	}
	rc = cluster_read_table(archive);
	if (rc == ZIM_ERR_RANGE && cluster.total &&
	    cluster.total <= ZIM_CLUSTER_LIMIT) {
		/* Rare: the cluster outgrew the guess.  Start over with an
		 * exactly sized buffer; the offset table is only a few KiB in. */
		rc = cluster_open(archive, cluster_start, cluster_end, extended,
				  cluster.total);
		if (!rc)
			rc = cluster_read_table(archive);
	}
	if (rc)
		goto fail;

	rc = ZIM_ERR_FORMAT;
	if (((uint64_t)dirent->blob_number + 2) * cluster.offset_size >
	    cluster.table_size)
		goto fail;
	table = cluster.output + (size_t)dirent->blob_number * cluster.offset_size;
	start = get_offset(table, cluster.offset_size);
	end = get_offset(table + cluster.offset_size, cluster.offset_size);
	if (start < cluster.table_size || end < start || end > cluster.total)
		goto fail;
	if (progress)
		progress(progress_opaque,
			 cluster.decoded < end ? cluster.decoded : end, end);
	rc = cluster_advance(archive, (size_t)end, progress, progress_opaque,
			     end);
	if (rc)
		goto fail;
	if (progress)
		progress(progress_opaque, end, end);
	*blob_start = (size_t)start;
	*blob_end = (size_t)end;
	return ZIM_OK;

fail:
	cluster_release();
	return rc;
}

static int read_zstd_blob(const ZIM_ARCHIVE *archive,
			  const ZIM_DIRENT *dirent,
			  uint64_t cluster_start, uint64_t cluster_end,
			  uint8_t extended,
			  void *buffer, size_t capacity,
			  size_t *blob_size,
			  ZIM_BLOB_PROGRESS progress,
			  void *progress_opaque)
{
	size_t blob_start;
	size_t blob_end;
	size_t amount;

	/* Whatever goes wrong with the cached whole-cluster decode, the
	 * windowed path is the authority: it either succeeds or reports why. */
	if (cached_blob(archive, dirent, cluster_start, cluster_end, extended,
			&blob_start, &blob_end, progress, progress_opaque))
		return read_zstd_blob_streaming(archive, dirent, cluster_start,
						cluster_end, extended, buffer,
						capacity, blob_size, progress,
						progress_opaque);
	*blob_size = blob_end - blob_start;
	amount = *blob_size < capacity ? *blob_size : capacity;
	if (amount)
		memcpy(buffer, cluster.output + blob_start, amount);
	return capacity < *blob_size ? ZIM_ERR_TRUNCATED : ZIM_OK;
}

int zim_archive_view_blob_progress(const ZIM_ARCHIVE *archive,
				   const ZIM_DIRENT *source_dirent,
				   const unsigned char **data,
				   size_t *blob_size,
				   ZIM_BLOB_PROGRESS progress,
				   void *progress_opaque)
{
	ZIM_DIRENT dirent;
	uint64_t cluster_start;
	uint64_t cluster_end;
	uint8_t compression;
	uint8_t extended;
	size_t blob_start;
	size_t blob_end;
	int rc;

	if (!archive || !source_dirent || !data || !blob_size)
		return ZIM_ERR_RANGE;
	*data = NULL;
	*blob_size = 0;
	dirent = *source_dirent;
	rc = zim_archive_resolve_redirect(archive, &dirent);
	if (rc)
		return rc;
	rc = zim_archive_blob_location(archive, &dirent, &cluster_start,
				       &cluster_end, &compression, &extended);
	if (rc)
		return rc;
	if (compression != 5)
		return ZIM_ERR_UNSUPPORTED;
	rc = cached_blob(archive, &dirent, cluster_start, cluster_end, extended,
			 &blob_start, &blob_end, progress, progress_opaque);
	if (rc)
		return rc;
	*data = cluster.output + blob_start;
	*blob_size = blob_end - blob_start;
	return ZIM_OK;
}

int zim_archive_read_blob_progress(const ZIM_ARCHIVE *archive,
				   const ZIM_DIRENT *source_dirent,
				   void *buffer, size_t capacity,
				   size_t *blob_size,
				   ZIM_BLOB_PROGRESS progress,
				   void *progress_opaque)
{
	ZIM_DIRENT dirent;
	uint64_t cluster_start;
	uint64_t cluster_end;
	uint8_t compression;
	uint8_t extended;
	int rc;

	if (!archive || !source_dirent || (!buffer && capacity) || !blob_size)
		return ZIM_ERR_RANGE;
	*blob_size = 0;
	dirent = *source_dirent;
	rc = zim_archive_resolve_redirect(archive, &dirent);
	if (rc)
		return rc;
	rc = zim_archive_blob_location(archive, &dirent, &cluster_start,
				       &cluster_end, &compression, &extended);
	if (rc)
		return rc;
	if (compression == 1)
		return read_uncompressed_blob(archive, &dirent, cluster_start,
					      cluster_end, extended, buffer,
					      capacity, blob_size, progress,
					      progress_opaque);
	if (compression == 5)
		return read_zstd_blob(archive, &dirent, cluster_start, cluster_end,
				      extended, buffer, capacity, blob_size,
				      progress, progress_opaque);
	return ZIM_ERR_UNSUPPORTED;
}

int zim_archive_read_blob(const ZIM_ARCHIVE *archive,
			  const ZIM_DIRENT *source_dirent,
			  void *buffer, size_t capacity,
			  size_t *blob_size)
{
	return zim_archive_read_blob_progress(archive, source_dirent, buffer,
					      capacity, blob_size, NULL, NULL);
}
