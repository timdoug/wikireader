/*
 * ZIM cluster decompression shared by the host diagnostics and WikiReader.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zim_blob.h"

#include <stdlib.h>
#include <string.h>

#define ZIM_REDIRECT_MIME 0xffff
#define ZIM_REDIRECT_LIMIT 64
#define ZIM_STREAM_BUFFER_SIZE (64 * 1024)
#define ZIM_OFFSET_TABLE_LIMIT (4 * 1024 * 1024)

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

extern ZSTD_DStream *ZSTD_createDStream(void);
extern size_t ZSTD_freeDStream(ZSTD_DStream *stream);
extern size_t ZSTD_initDStream(ZSTD_DStream *stream);
extern size_t ZSTD_decompressStream(ZSTD_DStream *stream,
				    ZSTD_outBuffer *output,
				    ZSTD_inBuffer *input);
extern unsigned ZSTD_isError(size_t code);

static uint32_t get_le32(const unsigned char *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
		(uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t get_le64(const unsigned char *p)
{
	return (uint64_t)get_le32(p) | (uint64_t)get_le32(p + 4) << 32;
}

static int read_exact(const ZIM_ARCHIVE *archive, uint64_t offset,
		      void *buffer, size_t length)
{
	if (offset > archive->io.size ||
	    (uint64_t)length > archive->io.size - offset)
		return ZIM_ERR_RANGE;
	return archive->io.read_at(archive->io.opaque, offset, buffer, length) ?
		ZIM_ERR_IO : ZIM_OK;
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
	start = extended ? get_le64(offsets) : get_le32(offsets);
	end = extended ? get_le64(offsets + 8) : get_le32(offsets + 4);
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

static int read_zstd_blob(const ZIM_ARCHIVE *archive,
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
	uint64_t first_offset;
	uint64_t table_need64;
	size_t offset_size = extended ? 8 : 4;
	size_t table_need;
	size_t table_have = 0;
	int range_known = 0;
	int result = ZIM_ERR_FORMAT;

	table_need64 = ((uint64_t)dirent->blob_number + 2) * offset_size;
	if (table_need64 > ZIM_OFFSET_TABLE_LIMIT ||
	    table_need64 > (uint64_t)(size_t)-1)
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
			compressed_pos += amount;
			input.src = input_buffer;
			input.size = amount;
			input.pos = 0;
		}

		output.dst = output_buffer;
		output.size = ZIM_STREAM_BUFFER_SIZE;
		output.pos = 0;
		remaining = ZSTD_decompressStream(stream, &output, &input);
		if (ZSTD_isError(remaining))
			goto out;

		if (!range_known && table_have < table_need) {
			size_t amount = output.pos;
			if (amount > table_need - table_have)
				amount = table_need - table_have;
			memcpy(offset_table + table_have, output_buffer, amount);
			table_have += amount;
			if (table_have == table_need) {
				first_offset = extended ? get_le64(offset_table) :
					get_le32(offset_table);
				blob_start = extended ?
					get_le64(offset_table +
						 (size_t)dirent->blob_number * 8) :
					get_le32(offset_table +
						 (size_t)dirent->blob_number * 4);
				blob_end = extended ?
					get_le64(offset_table +
						 ((size_t)dirent->blob_number + 1) * 8) :
					get_le32(offset_table +
						 ((size_t)dirent->blob_number + 1) * 4);
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
			if (blob_start == blob_end ||
			    completed >= blob_end) {
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
