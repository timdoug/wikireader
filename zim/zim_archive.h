/* Small, allocation-free reader for ZIM format 6 archives. */
#ifndef WIKIREADER_ZIM_ARCHIVE_H
#define WIKIREADER_ZIM_ARCHIVE_H

#include <inttypes.h>
#include <stddef.h>

#define ZIM_DIRENT_TEXT_MAX 512

typedef int (*zim_read_at_fn)(void *opaque, uint64_t offset, void *buffer,
			      size_t length);

typedef struct {
	zim_read_at_fn read_at;
	void *opaque;
	uint64_t size;
} ZIM_IO;

typedef struct {
	ZIM_IO io;
	uint16_t major_version;
	uint16_t minor_version;
	uint32_t entry_count;
	uint32_t cluster_count;
	uint64_t path_ptr_pos;
	uint64_t title_ptr_pos;
	uint64_t cluster_ptr_pos;
	uint64_t mime_list_pos;
	uint32_t main_page;
	uint64_t checksum_pos;
	uint64_t title_listing_pos;
	uint32_t title_listing_count;
} ZIM_ARCHIVE;

typedef struct {
	uint16_t mime_type;
	uint8_t parameter_length;
	char name_space;
	uint32_t revision;
	uint32_t cluster_number;
	uint32_t blob_number;
	uint32_t redirect_index;
	uint32_t path_index;
	char path[ZIM_DIRENT_TEXT_MAX];
	char title[ZIM_DIRENT_TEXT_MAX];
} ZIM_DIRENT;

enum {
	ZIM_OK = 0,
	ZIM_ERR_IO = -1,
	ZIM_ERR_FORMAT = -2,
	ZIM_ERR_RANGE = -3,
	ZIM_ERR_UNSUPPORTED = -4,
	ZIM_ERR_NOT_FOUND = -5,
	ZIM_ERR_TRUNCATED = -6
};

int zim_archive_open(ZIM_ARCHIVE *archive, const ZIM_IO *io);
int zim_archive_read_dirent(const ZIM_ARCHIVE *archive, uint32_t path_index,
			    ZIM_DIRENT *dirent);
int zim_archive_find_path(const ZIM_ARCHIVE *archive, char name_space,
			  const char *path, ZIM_DIRENT *dirent);
int zim_archive_title_at(const ZIM_ARCHIVE *archive, uint32_t title_index,
			 ZIM_DIRENT *dirent);
int zim_archive_find_title_prefix(const ZIM_ARCHIVE *archive,
				  const char *prefix,
				  uint32_t *first_title_index);
int zim_archive_blob_location(const ZIM_ARCHIVE *archive,
			      const ZIM_DIRENT *dirent,
			      uint64_t *cluster_start,
			      uint64_t *cluster_end,
			      uint8_t *compression,
			      uint8_t *extended);
const char *zim_error_string(int error);

#endif
