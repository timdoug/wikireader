/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "zim_fat.h"

#include <stdlib.h>
#include <string.h>

#define FAT_SECTOR_SIZE 512
#define FAT32_END 0x0ffffff8UL

static uint16_t get_le16(const unsigned char *p)
{
	return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}

static uint32_t get_le32(const unsigned char *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
		(uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int read_one(const ZIM_FAT_VOLUME *volume, uint32_t sector,
		    unsigned char data[FAT_SECTOR_SIZE])
{
	return volume->read_sectors(volume->opaque, sector, data, 1);
}

static int valid_bpb(const unsigned char sector[FAT_SECTOR_SIZE])
{
	return get_le16(sector + 11) == FAT_SECTOR_SIZE &&
		sector[13] && !(sector[13] & (sector[13] - 1)) &&
		get_le16(sector + 14) && sector[16] && get_le32(sector + 36) &&
		get_le32(sector + 44) >= 2;
}

static int open_volume(ZIM_FAT_VOLUME *volume,
		       zim_sector_read_fn read_sectors, void *opaque)
{
	unsigned char sector[FAT_SECTOR_SIZE];
	uint32_t volume_start = 0;
	uint32_t fat_size;
	uint32_t root_directory_sectors;
	uint32_t total_fats;

	memset(volume, 0, sizeof(*volume));
	volume->read_sectors = read_sectors;
	volume->opaque = opaque;
	if (read_sectors(opaque, 0, sector, 1))
		return -1;
	if (!valid_bpb(sector)) {
		/* The emulator also accepts partitioned cards. Use the first MBR
		 * entry with a nonzero start sector. */
		int entry;
		for (entry = 0; entry < 4; entry++) {
			volume_start = get_le32(sector + 446 + entry * 16 + 8);
			if (volume_start)
				break;
		}
		if (!volume_start || read_sectors(opaque, volume_start, sector, 1) ||
		    !valid_bpb(sector))
			return -1;
	}
	volume->volume_start = volume_start;
	volume->sectors_per_cluster = sector[13];
	volume->root_cluster = get_le32(sector + 44);
	fat_size = get_le32(sector + 36);
	volume->sectors_per_fat = fat_size;
	volume->fat_start = volume_start + get_le16(sector + 14);
	total_fats = (uint32_t)sector[16] * fat_size;
	root_directory_sectors =
		((uint32_t)get_le16(sector + 17) * 32 + FAT_SECTOR_SIZE - 1) /
		FAT_SECTOR_SIZE;
	volume->data_start = volume->fat_start + total_fats +
		root_directory_sectors;
	return 0;
}

static int next_cluster(ZIM_FAT_VOLUME *volume, uint32_t cluster,
			uint32_t *next)
{
	uint32_t byte_offset = cluster * 4;
	uint32_t sector = volume->fat_start + byte_offset / FAT_SECTOR_SIZE;
	if (!volume->fat_cache_valid || volume->cached_fat_sector != sector) {
		if (read_one(volume, sector, volume->fat_cache))
			return -1;
		volume->cached_fat_sector = sector;
		volume->fat_cache_valid = 1;
	}
	*next = get_le32(volume->fat_cache + byte_offset % FAT_SECTOR_SIZE) &
		0x0fffffffUL;
	return 0;
}

static uint32_t cluster_sector(const ZIM_FAT_VOLUME *volume, uint32_t cluster)
{
	return volume->data_start +
		(cluster - 2) * volume->sectors_per_cluster;
}

static int find_entry(ZIM_FAT_VOLUME *volume, uint32_t directory_cluster,
		      const char name[11], uint32_t *entry_cluster,
		      uint32_t *entry_size, unsigned char *attributes)
{
	unsigned char sector[FAT_SECTOR_SIZE];
	uint32_t cluster = directory_cluster;
	uint32_t guard = 0;

	while (cluster >= 2 && cluster < FAT32_END && guard++ < 0x100000) {
		uint32_t s;
		for (s = 0; s < volume->sectors_per_cluster; s++) {
			unsigned int offset;
			if (read_one(volume, cluster_sector(volume, cluster) + s, sector))
				return -1;
			for (offset = 0; offset < FAT_SECTOR_SIZE; offset += 32) {
				const unsigned char *entry = sector + offset;
				if (entry[0] == 0)
					return 1;
				if (entry[0] == 0xe5 || entry[11] == 0x0f ||
				    (entry[11] & 0x08))
					continue;
				if (!memcmp(entry, name, 11)) {
					*entry_cluster =
						(uint32_t)get_le16(entry + 20) << 16 |
						get_le16(entry + 26);
					*entry_size = get_le32(entry + 28);
					*attributes = entry[11];
					return 0;
				}
			}
		}
		if (next_cluster(volume, cluster, &cluster))
			return -1;
	}
	return 1;
}

int zim_fat_open_83(ZIM_FAT_FILE *file, zim_sector_read_fn read_sectors,
		    void *opaque, const char directory[11],
		    const char filename[11])
{
	uint32_t directory_cluster;
	uint32_t ignored_size;
	uint32_t file_cluster;
	uint32_t cluster_bytes;
	uint32_t i;
	unsigned char attributes;

	if (!file || !read_sectors || !directory || !filename)
		return -1;
	memset(file, 0, sizeof(*file));
	if (open_volume(&file->volume, read_sectors, opaque))
		return -1;
	if (find_entry(&file->volume, file->volume.root_cluster, directory,
		       &directory_cluster, &ignored_size, &attributes) ||
	    !(attributes & 0x10))
		return -1;
	if (find_entry(&file->volume, directory_cluster, filename,
		       &file_cluster, &file->size, &attributes) ||
	    (attributes & 0x10) || file_cluster < 2)
		return -1;
	cluster_bytes = file->volume.sectors_per_cluster * FAT_SECTOR_SIZE;
	file->cluster_count = (uint32_t)(((uint64_t)file->size +
		cluster_bytes - 1) / cluster_bytes);
	if (!file->cluster_count)
		return -1;
	file->clusters = malloc((size_t)file->cluster_count * sizeof(uint32_t));
	if (!file->clusters)
		return -1;
	for (i = 0; i < file->cluster_count; i++) {
		if (file_cluster < 2 || file_cluster >= FAT32_END) {
			zim_fat_close(file);
			return -1;
		}
		file->clusters[i] = file_cluster;
		if (i + 1 < file->cluster_count &&
		    next_cluster(&file->volume, file_cluster, &file_cluster)) {
			zim_fat_close(file);
			return -1;
		}
	}
	return 0;
}

int zim_fat_read_at(const ZIM_FAT_FILE *file, uint64_t offset,
		    void *destination, size_t length)
{
	unsigned char scratch[FAT_SECTOR_SIZE];
	unsigned char *output = destination;
	uint32_t sector_index;
	uint32_t sector_offset;

	if (!file || !file->clusters || (!destination && length) ||
	    offset > file->size || (uint64_t)length > file->size - offset)
		return -1;
	sector_index = (uint32_t)(offset / FAT_SECTOR_SIZE);
	sector_offset = (uint32_t)(offset % FAT_SECTOR_SIZE);
	while (length) {
		uint32_t cluster_index = sector_index /
			file->volume.sectors_per_cluster;
		uint32_t in_cluster = sector_index %
			file->volume.sectors_per_cluster;
		uint32_t sector;
		size_t amount;
		if (cluster_index >= file->cluster_count)
			return -1;
		sector = cluster_sector(&file->volume, file->clusters[cluster_index]) +
			in_cluster;
		if (!sector_offset && length >= FAT_SECTOR_SIZE) {
			uint32_t count = file->volume.sectors_per_cluster - in_cluster;
			if (count > length / FAT_SECTOR_SIZE)
				count = (uint32_t)(length / FAT_SECTOR_SIZE);
			if (file->volume.read_sectors(file->volume.opaque, sector,
						      output, count))
				return -1;
			amount = (size_t)count * FAT_SECTOR_SIZE;
			sector_index += count;
		} else {
			if (read_one(&file->volume, sector, scratch))
				return -1;
			amount = FAT_SECTOR_SIZE - sector_offset;
			if (amount > length)
				amount = length;
			memcpy(output, scratch + sector_offset, amount);
			sector_offset = 0;
			sector_index++;
		}
		output += amount;
		length -= amount;
	}
	return 0;
}

void zim_fat_close(ZIM_FAT_FILE *file)
{
	if (!file)
		return;
	free(file->clusters);
	file->clusters = NULL;
	file->cluster_count = 0;
	file->size = 0;
}
