#define _FILE_OFFSET_BITS 64
#define _DARWIN_C_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zim_archive.h"
#include "zim_fat.h"

typedef struct { FILE *file; } IMAGE_IO;

static int read_sectors(void *opaque, uint32_t sector, void *buffer,
			uint32_t count)
{
	IMAGE_IO *image = opaque;
	size_t length = (size_t)count * 512;
	if (fseeko(image->file, (off_t)sector * 512, SEEK_SET))
		return -1;
	return fread(buffer, 1, length, image->file) == length ? 0 : -1;
}

static int read_at(void *opaque, uint64_t offset, void *buffer, size_t length)
{
	return zim_fat_read_at(opaque, offset, buffer, length);
}

int main(int argc, char **argv)
{
	IMAGE_IO image;
	ZIM_FAT_FILE file;
	ZIM_IO io;
	ZIM_ARCHIVE archive;
	ZIM_DIRENT dirent;
	uint32_t first;
	int rc;

	if (argc != 3) {
		fprintf(stderr, "usage: %s FAT-IMAGE TITLE-PREFIX\n", argv[0]);
		return 2;
	}
	image.file = fopen(argv[1], "rb");
	if (!image.file) {
		fprintf(stderr, "%s: %s\n", argv[1], strerror(errno));
		return 1;
	}
	if (zim_fat_open_83(&file, read_sectors, &image,
			    "ZIM        ", "WIKI    ZIM")) {
		fprintf(stderr, "%s: cannot map zim/wiki.zim\n", argv[1]);
		fclose(image.file);
		return 1;
	}
	io.read_at = read_at;
	io.opaque = &file;
	io.size = file.size;
	rc = zim_archive_open(&archive, &io);
	if (!rc)
		rc = zim_archive_find_title_prefix(&archive, argv[2], &first);
	if (!rc)
		rc = zim_archive_title_at(&archive, first, &dirent);
	if (rc && rc != ZIM_ERR_TRUNCATED) {
		fprintf(stderr, "%s\n", zim_error_string(rc));
		zim_fat_close(&file);
		fclose(image.file);
		return 1;
	}
	printf("size=%" PRIu32 " clusters=%" PRIu32
	       " entries=%" PRIu32 " titles=%" PRIu32 " first=%s\n",
	       file.size, file.cluster_count, archive.entry_count,
	       archive.title_listing_count, dirent.title);
	zim_fat_close(&file);
	fclose(image.file);
	return 0;
}
