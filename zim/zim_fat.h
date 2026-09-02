/* Read-only FAT32 file mapping for efficient random access to large archives. */
#ifndef WIKIREADER_ZIM_FAT_H
#define WIKIREADER_ZIM_FAT_H

#include <inttypes.h>
#include <stddef.h>

#define ZIM_FAT_TABLE_CACHE_SECTORS 8
#define ZIM_FAT_DATA_CACHE_COUNT 4

typedef int (*zim_sector_read_fn)(void *opaque, uint32_t sector,
				  void *buffer, uint32_t count);
typedef void (*zim_fat_progress_fn)(void *opaque, uint32_t completed,
				    uint32_t total);

typedef struct {
	zim_sector_read_fn read_sectors;
	void *opaque;
	uint32_t volume_start;
	uint32_t fat_start;
	uint32_t data_start;
	uint32_t root_cluster;
	uint32_t sectors_per_cluster;
	uint32_t sectors_per_fat;
	uint32_t cached_fat_byte_offset;
	uint32_t cached_fat_bytes;
	unsigned char fat_cache[ZIM_FAT_TABLE_CACHE_SECTORS * 512];
} ZIM_FAT_VOLUME;

typedef struct {
	ZIM_FAT_VOLUME volume;
	uint32_t *clusters;
	uint32_t cluster_count;
	uint32_t size;
	uint32_t cached_data_sector[ZIM_FAT_DATA_CACHE_COUNT];
	unsigned char data_cache[ZIM_FAT_DATA_CACHE_COUNT][512];
	unsigned char data_cache_valid[ZIM_FAT_DATA_CACHE_COUNT];
	unsigned char next_data_cache;
} ZIM_FAT_FILE;

int zim_fat_open_83(ZIM_FAT_FILE *file, zim_sector_read_fn read_sectors,
		    void *opaque, const char directory[11],
		    const char filename[11]);
int zim_fat_open_83_progress(ZIM_FAT_FILE *file,
			     zim_sector_read_fn read_sectors, void *opaque,
			     const char directory[11], const char filename[11],
			     zim_fat_progress_fn progress,
			     void *progress_opaque);
int zim_fat_read_at(ZIM_FAT_FILE *file, uint64_t offset,
		    void *buffer, size_t length);
void zim_fat_close(ZIM_FAT_FILE *file);

#endif
