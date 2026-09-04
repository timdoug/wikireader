/*
 * Check the decoded-cluster cache against an independent whole-cluster decode.
 *
 * For a sample of articles this reads the requested blob, a neighbour that is
 * already below the decoded frontier, two neighbours that require the live
 * stream to continue, a truncated read, and the zero-copy view.  Each result
 * must match the bytes produced by decoding the whole cluster in one pass with
 * the decoder's ordinary buffered mode.
 *
 * usage: blob-cache-test ARCHIVE [SAMPLES]
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zim_archive.h"
#include "zim_blob.h"

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

#define REFERENCE_LIMIT (16u << 20)
#define READ_LIMIT (4u << 20)

typedef struct {
	FILE *file;
} HOST_IO;

static int host_read(void *opaque, uint64_t offset, void *buffer,
		     size_t length)
{
	HOST_IO *io = opaque;
	if (fseeko(io->file, (off_t)offset, SEEK_SET))
		return -1;
	return fread(buffer, 1, length, io->file) == length ? 0 : -1;
}

static uint32_t get_le32(const unsigned char *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
		(uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Decode a whole compressed cluster through the buffered streaming API. */
static unsigned char *reference_cluster(HOST_IO *io, uint64_t cluster_start,
					uint64_t cluster_end, size_t *size)
{
	size_t compressed = (size_t)(cluster_end - cluster_start - 1);
	unsigned char *input = malloc(compressed);
	unsigned char *output = malloc(REFERENCE_LIMIT);
	ZSTD_DStream *stream = ZSTD_createDStream();
	ZSTD_inBuffer in = { input, compressed, 0 };
	ZSTD_outBuffer out = { output, REFERENCE_LIMIT, 0 };
	size_t rc = 1;

	if (!input || !output || !stream ||
	    host_read(io, cluster_start + 1, input, compressed)) {
		free(output);
		output = NULL;
		goto out;
	}
	ZSTD_initDStream(stream);
	while (rc && !ZSTD_isError(rc) && in.pos < in.size)
		rc = ZSTD_decompressStream(stream, &out, &in);
	if (ZSTD_isError(rc) || rc) {
		free(output);
		output = NULL;
	}
	*size = out.pos;
out:
	ZSTD_freeDStream(stream);
	free(input);
	return output;
}

static int check_blob(const ZIM_ARCHIVE *archive, const ZIM_DIRENT *dirent,
		      const unsigned char *reference, unsigned char *buffer,
		      const char *what)
{
	uint32_t start = get_le32(reference + 4 * dirent->blob_number);
	uint32_t end = get_le32(reference + 4 * (dirent->blob_number + 1));
	unsigned char small[100];
	const unsigned char *view;
	size_t size;
	int rc;

	rc = zim_archive_read_blob(archive, dirent, buffer, READ_LIMIT, &size);
	if (rc || size != end - start || memcmp(buffer, reference + start, size)) {
		printf("FAIL %s: %s blob %u read (rc %d)\n", what, dirent->path,
		       dirent->blob_number, rc);
		return 1;
	}
	rc = zim_archive_read_blob(archive, dirent, small, sizeof(small), &size);
	if (size > sizeof(small) ?
	    (rc != ZIM_ERR_TRUNCATED ||
	     memcmp(small, reference + start, sizeof(small))) :
	    (rc || memcmp(small, reference + start, size))) {
		printf("FAIL %s: %s blob %u truncated read (rc %d)\n", what,
		       dirent->path, dirent->blob_number, rc);
		return 1;
	}
	rc = zim_archive_view_blob_progress(archive, dirent, &view, &size,
					    NULL, NULL);
	if (rc == ZIM_ERR_UNSUPPORTED)
		return 0;
	if (rc || size != end - start || memcmp(view, reference + start, size)) {
		printf("FAIL %s: %s blob %u view (rc %d)\n", what, dirent->path,
		       dirent->blob_number, rc);
		return 1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	static const int neighbours[] = { 0, -1, 1, 2 };
	static const char *const names[] = {
		"requested", "before frontier", "continue", "continue again"
	};
	HOST_IO io;
	ZIM_ARCHIVE archive;
	ZIM_IO zim_io;
	unsigned char *buffer;
	unsigned int samples = argc > 2 ? (unsigned int)atoi(argv[2]) : 40;
	unsigned int checked = 0;
	unsigned int failures = 0;
	unsigned int k;

	if (argc < 2) {
		fprintf(stderr, "usage: %s ARCHIVE [SAMPLES]\n", argv[0]);
		return 2;
	}
	io.file = fopen(argv[1], "rb");
	if (!io.file) {
		perror(argv[1]);
		return 2;
	}
	fseeko(io.file, 0, SEEK_END);
	zim_io.size = (uint64_t)ftello(io.file);
	zim_io.opaque = &io;
	zim_io.read_at = host_read;
	if (zim_archive_open(&archive, &zim_io)) {
		fprintf(stderr, "%s: not a supported ZIM archive\n", argv[1]);
		return 2;
	}
	buffer = malloc(READ_LIMIT);
	if (!buffer)
		return 2;
	srand(1);
	for (k = 0; k < samples; k++) {
		ZIM_DIRENT dirent;
		uint64_t cluster_start;
		uint64_t cluster_end;
		uint8_t compression;
		uint8_t extended;
		unsigned char *reference;
		size_t reference_size;
		uint32_t blob_count;
		unsigned int n;
		int rc;

		rc = zim_archive_read_dirent(&archive,
					     (uint32_t)rand() % archive.entry_count,
					     &dirent);
		if ((rc && rc != ZIM_ERR_TRUNCATED) ||
		    zim_archive_resolve_redirect(&archive, &dirent) ||
		    dirent.name_space != 'C' ||
		    zim_archive_blob_location(&archive, &dirent, &cluster_start,
					      &cluster_end, &compression,
					      &extended) ||
		    compression != 5 || extended)
			continue;
		reference = reference_cluster(&io, cluster_start, cluster_end,
					      &reference_size);
		if (!reference) {
			printf("FAIL: cluster %u reference decode\n",
			       dirent.cluster_number);
			failures++;
			continue;
		}
		blob_count = get_le32(reference) / 4 - 1;
		for (n = 0; n < sizeof(neighbours) / sizeof(neighbours[0]); n++) {
			ZIM_DIRENT probe = dirent;
			long blob = (long)dirent.blob_number + neighbours[n];

			if (blob < 0 || blob >= (long)blob_count)
				continue;
			probe.blob_number = (uint32_t)blob;
			failures += (unsigned int)check_blob(&archive, &probe,
							     reference, buffer,
							     names[n]);
			checked++;
		}
		free(reference);
	}
	free(buffer);
	fclose(io.file);
	if (failures || !checked) {
		printf("FAIL: %u of %u blob checks failed\n", failures, checked);
		return 1;
	}
	printf("PASS: %u cached blob reads matched reference decodes\n", checked);
	return 0;
}
