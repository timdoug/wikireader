#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zim_fat.h"

typedef struct {
	unsigned int reads;
} TEST_IO;

static int read_sectors(void *opaque, uint32_t sector, void *buffer,
			uint32_t count)
{
	TEST_IO *io = opaque;
	unsigned char *bytes = buffer;
	uint32_t i;
	uint32_t j;

	io->reads++;
	for (i = 0; i < count; i++)
		for (j = 0; j < 512; j++)
			bytes[i * 512 + j] = (unsigned char)(sector + i + j);
	return 0;
}

static int read_byte(ZIM_FAT_FILE *file, uint64_t offset,
		     unsigned char expected)
{
	unsigned char byte = 0;
	return zim_fat_read_at(file, offset, &byte, 1) || byte != expected;
}

int main(void)
{
	ZIM_FAT_FILE file;
	TEST_IO io = { 0 };
	unsigned char crossing[4];
	uint32_t i;

	memset(&file, 0, sizeof(file));
	file.volume.read_sectors = read_sectors;
	file.volume.opaque = &io;
	file.volume.data_start = 100;
	file.volume.sectors_per_cluster = 1;
	file.cluster_count = 5;
	file.size = 5 * 512;
	file.clusters = malloc(file.cluster_count * sizeof(*file.clusters));
	if (!file.clusters)
		return 1;
	for (i = 0; i < file.cluster_count; i++)
		file.clusters[i] = i + 2;

	if (read_byte(&file, 0, 100) || read_byte(&file, 1, 101) ||
	    io.reads != 1 || read_byte(&file, 512, 101) ||
	    read_byte(&file, 1024, 102) || read_byte(&file, 1536, 103) ||
	    io.reads != 4 || read_byte(&file, 0, 100) || io.reads != 4 ||
	    read_byte(&file, 2048, 104) || io.reads != 5 ||
	    read_byte(&file, 0, 100) || io.reads != 6 ||
	    zim_fat_read_at(&file, 510, crossing, sizeof(crossing)) ||
	    crossing[0] != 98 || crossing[1] != 99 ||
	    crossing[2] != 101 || crossing[3] != 102 || io.reads != 7 ||
	    !zim_fat_read_at(&file, file.size, crossing, 1)) {
		fprintf(stderr, "FAT data-cache regression\n");
		zim_fat_close(&file);
		return 1;
	}
	zim_fat_close(&file);
	puts("PASS: FAT data-sector cache");
	return 0;
}
