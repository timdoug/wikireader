#define _FILE_OFFSET_BITS 64
#define _DARWIN_C_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zim_archive.h"
#include "zim_blob.h"
#include "zim_html.h"
#include "zim_image.h"

typedef struct {
	FILE *file;
} HOST_IO;

static int host_read_at(void *opaque, uint64_t offset, void *buffer,
			size_t length)
{
	HOST_IO *host = opaque;
	if (offset > 0x7fffffffffffffffULL ||
	    fseeko(host->file, (off_t)offset, SEEK_SET))
		return -1;
	return fread(buffer, 1, length, host->file) == length ? 0 : -1;
}

static void usage(const char *name)
{
	fprintf(stderr,
		"usage: %s ARCHIVE info\n"
		"       %s ARCHIVE title TITLE-PREFIX [COUNT]\n"
		"       %s ARCHIVE path NAMESPACE PATH\n"
		"       %s ARCHIVE blob NAMESPACE PATH\n"
		"       %s ARCHIVE text NAMESPACE PATH\n"
		"       %s ARCHIVE image-info NAMESPACE PATH\n",
		name, name, name, name, name, name);
}

static void print_dirent(uint32_t ordinal, const ZIM_DIRENT *dirent)
{
	printf("%" PRIu32 "\t%" PRIu32 "\t%c/%s\t%s\t",
	       ordinal, dirent->path_index, dirent->name_space,
	       dirent->path, dirent->title);
	if (dirent->mime_type == 0xffff)
		printf("redirect=%" PRIu32 "\n", dirent->redirect_index);
	else
		printf("cluster=%" PRIu32 " blob=%" PRIu32 " mime=%u\n",
		       dirent->cluster_number, dirent->blob_number,
		       dirent->mime_type);
}

int main(int argc, char **argv)
{
	HOST_IO host;
	ZIM_IO io;
	ZIM_ARCHIVE archive;
	off_t size;
	int rc;
	int status = 1;

	if (argc < 3) {
		usage(argv[0]);
		return 2;
	}
	host.file = fopen(argv[1], "rb");
	if (!host.file) {
		fprintf(stderr, "%s: %s\n", argv[1], strerror(errno));
		return 1;
	}
	if (fseeko(host.file, 0, SEEK_END) || (size = ftello(host.file)) < 0) {
		fprintf(stderr, "%s: cannot determine size\n", argv[1]);
		goto out;
	}
	io.read_at = host_read_at;
	io.opaque = &host;
	io.size = (uint64_t)size;
	rc = zim_archive_open(&archive, &io);
	if (rc) {
		fprintf(stderr, "%s: %s\n", argv[1], zim_error_string(rc));
		goto out;
	}

	if (!strcmp(argv[2], "info") && argc == 3) {
		printf("ZIM %u.%u\n", archive.major_version, archive.minor_version);
		printf("size:             %" PRIu64 "\n", archive.io.size);
		printf("entries:          %" PRIu32 "\n", archive.entry_count);
		printf("clusters:         %" PRIu32 "\n", archive.cluster_count);
		printf("path pointers:    %" PRIu64 "\n", archive.path_ptr_pos);
		printf("cluster pointers: %" PRIu64 "\n", archive.cluster_ptr_pos);
		printf("main page:        %" PRIu32 "\n", archive.main_page);
		printf("title listing:    %" PRIu64 " (%" PRIu32 " entries)\n",
		       archive.title_listing_pos, archive.title_listing_count);
		status = 0;
	} else if (!strcmp(argv[2], "title") && (argc == 4 || argc == 5)) {
		uint32_t first;
		uint32_t count = argc == 5 ? (uint32_t)strtoul(argv[4], NULL, 0) : 10;
		uint32_t i;
		rc = zim_archive_find_title_prefix(&archive, argv[3], &first);
		if (rc) {
			fprintf(stderr, "%s: %s\n", argv[3], zim_error_string(rc));
			goto out;
		}
		for (i = 0; i < count && first + i < archive.title_listing_count; i++) {
			ZIM_DIRENT dirent;
			rc = zim_archive_title_at(&archive, first + i, &dirent);
			if (rc && rc != ZIM_ERR_TRUNCATED) {
				fprintf(stderr, "title entry: %s\n", zim_error_string(rc));
				goto out;
			}
			if (strncmp(dirent.title, argv[3], strlen(argv[3])))
				break;
			print_dirent(first + i, &dirent);
		}
		status = 0;
	} else if (!strcmp(argv[2], "path") && argc == 5 && strlen(argv[3]) == 1) {
		ZIM_DIRENT dirent;
		rc = zim_archive_find_path(&archive, argv[3][0], argv[4], &dirent);
		if (rc && rc != ZIM_ERR_TRUNCATED) {
			fprintf(stderr, "%c/%s: %s\n", argv[3][0], argv[4],
				zim_error_string(rc));
			goto out;
		}
		print_dirent(dirent.path_index, &dirent);
		status = 0;
	} else if ((!strcmp(argv[2], "blob") || !strcmp(argv[2], "text") ||
		    !strcmp(argv[2], "image-info")) &&
		   argc == 5 && strlen(argv[3]) == 1) {
		ZIM_DIRENT dirent;
		unsigned char *blob;
		size_t blob_size;
		int as_text = !strcmp(argv[2], "text");
		int as_image = !strcmp(argv[2], "image-info");
		rc = zim_archive_find_path(&archive, argv[3][0], argv[4], &dirent);
		if (rc && rc != ZIM_ERR_TRUNCATED) {
			fprintf(stderr, "%c/%s: %s\n", argv[3][0], argv[4],
				zim_error_string(rc));
			goto out;
		}
		rc = zim_archive_read_blob(&archive, &dirent, NULL, 0, &blob_size);
		if (rc != ZIM_OK && rc != ZIM_ERR_TRUNCATED) {
			fprintf(stderr, "%c/%s: %s\n", argv[3][0], argv[4],
				zim_error_string(rc));
			goto out;
		}
		blob = malloc(blob_size ? blob_size : 1);
		if (!blob) {
			fprintf(stderr, "cannot allocate %zu-byte blob\n", blob_size);
			goto out;
		}
		rc = zim_archive_read_blob(&archive, &dirent, blob, blob_size,
					   &blob_size);
		if (rc) {
			fprintf(stderr, "%c/%s: %s\n", argv[3][0], argv[4],
				zim_error_string(rc));
			free(blob);
			goto out;
		}
		if (as_image) {
			unsigned char bitmap[((226 + 7) / 8) * 280];
			uint8_t width;
			uint16_t height;
			size_t bitmap_size;
			size_t i;
			size_t black = 0;

			rc = zim_webp_to_bitmap(blob, blob_size, 226, 0, bitmap,
						 sizeof(bitmap), &width, &height,
						 &bitmap_size);
			free(blob);
			if (rc) {
				fprintf(stderr, "%c/%s: WebP decode failed\n",
					argv[3][0], argv[4]);
				goto out;
			}
			for (i = 0; i < bitmap_size; i++) {
				unsigned char bits = bitmap[i];
				while (bits) {
					black += bits & 1;
					bits >>= 1;
				}
			}
			printf("%u x %u, %zu bytes, %zu black pixels\n",
			       (unsigned int)width, (unsigned int)height,
			       bitmap_size, black);
			status = 0;
			goto out;
		} else if (as_text) {
			unsigned char *plain = malloc(blob_size + 1);
			size_t plain_size;
			if (!plain) {
				fprintf(stderr, "cannot allocate article text buffer\n");
				free(blob);
				goto out;
			}
			rc = zim_html_to_text(blob, blob_size, plain, blob_size + 1,
					      &plain_size);
			free(blob);
			if (rc) {
				fprintf(stderr, "%c/%s: HTML conversion: %s\n",
					argv[3][0], argv[4], zim_error_string(rc));
				free(plain);
				goto out;
			}
			blob = plain;
			blob_size = plain_size;
		}
		if (fwrite(blob, 1, blob_size, stdout) != blob_size) {
			fprintf(stderr, "cannot write decoded blob\n");
			free(blob);
			goto out;
		}
		free(blob);
		status = 0;
	} else {
		usage(argv[0]);
		status = 2;
	}

out:
	fclose(host.file);
	return status;
}
