/* Random-access adapter for a ZIM archive stored through Grifo's file API. */
#ifndef WIKIREADER_ZIM_FILE_H
#define WIKIREADER_ZIM_FILE_H

#include <inttypes.h>
#include <stddef.h>

#define ZIM_FILE_PAGE_SIZE 512
#define ZIM_FILE_CACHE_PAGES 4

typedef struct {
	int handle;
	uint64_t size;
	uint32_t *link_map;
	uint32_t cached_page[ZIM_FILE_CACHE_PAGES];
	unsigned char cache[ZIM_FILE_CACHE_PAGES][ZIM_FILE_PAGE_SIZE];
	unsigned char cache_valid[ZIM_FILE_CACHE_PAGES];
	unsigned char next_cache;
	unsigned char open;
} ZIM_FILE;

int zim_file_open(ZIM_FILE *file, const char *path);
int zim_file_read_at(ZIM_FILE *file, uint64_t offset, void *buffer,
		     size_t length);
void zim_file_close(ZIM_FILE *file);

#endif
