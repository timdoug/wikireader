/* Exercise the production FatFs seek-map walk without reading an archive.
 * A synthetic 124 GB file and slow metadata reads model the startup hazard. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ff.h"
#include "diskio.h"

#define FAT_ENTRIES 1048576U
#define FAT_BASE 8U
#define CLUSTER_BYTES 131072ULL
#define ARCHIVE_BYTES 123980647016ULL
#define ARCHIVE_CLUSTERS ((DWORD)((ARCHIVE_BYTES + CLUSTER_BYTES - 1) / CLUSTER_BYTES))

static unsigned char fat[FAT_ENTRIES * 4];
static unsigned elapsed_ms, last_service_ms, service_count, read_count;
static unsigned sector_count, write_count;
static int service_enabled, timed_out;
static LBA_t fail_sector;
static FILE *snapshot;
static long snapshot_size;
static int allow_window_flush;
PARTITION VolToPart[FF_VOLUMES] = {{0, 0}, {0, 2}};

void ff_fastseek_progress(void)
{
	if (service_enabled) {
		last_service_ms = elapsed_ms;
		++service_count;
	}
}

DSTATUS mmc_disk_initialize(BYTE drive) { (void)drive; return 0; }
DSTATUS mmc_disk_status(BYTE drive) { (void)drive; return 0; }
DRESULT mmc_disk_ioctl(BYTE drive, BYTE command, void *buffer)
{
	(void)drive; (void)command; (void)buffer;
	return RES_OK;
}
DRESULT mmc_disk_write(BYTE drive, const BYTE *buffer, DWORD sector, BYTE count)
{
	(void)drive;
	assert(allow_window_flush && !snapshot);
	assert(count == 1 && sector >= FAT_BASE);
	assert(sector - FAT_BASE < sizeof(fat) / 512);
	memcpy(fat + (sector - FAT_BASE) * 512, buffer, 512);
	++write_count;
	return RES_OK;
}
DRESULT mmc_disk_read(BYTE drive, BYTE *buffer, DWORD sector, BYTE count)
{
	(void)drive;
	assert(count > 0);
	++read_count;
	sector_count += count;
	elapsed_ms += 10 * count;
	if (elapsed_ms - last_service_ms > 20000) {
		timed_out = 1;
		return RES_ERROR;
	}
	if (sector <= fail_sector && fail_sector - sector < count)
		return RES_ERROR;
	if (snapshot) {
		/* Never substitute zero-filled archive data for missing metadata. */
		assert(((uint64_t)sector + count) * 512 <= (uint64_t)snapshot_size);
		assert(fseek(snapshot, (long)sector * 512, SEEK_SET) == 0);
		assert(fread(buffer, 512, count, snapshot) == count);
		return RES_OK;
	}
	assert(sector >= FAT_BASE);
	assert(sector - FAT_BASE + count <= sizeof(fat) / 512);
	memcpy(buffer, fat + (sector - FAT_BASE) * 512, count * 512);
	return RES_OK;
}

static void set_fat(DWORD cluster, DWORD value)
{
	unsigned i;
	assert(cluster < FAT_ENTRIES);
	for (i = 0; i < 4; ++i)
		fat[cluster * 4 + i] = (BYTE)(value >> (8 * i));
}

static void make_chain(void)
{
	DWORD i;
	memset(fat, 0, sizeof(fat));
	for (i = 0; i < 400000; ++i)
		set_fat(2 + i, i + 1 == 400000 ? 450005 : 3 + i);
	for (i = 0; i < 400000; ++i)
		set_fat(450005 + i, i + 1 == 400000 ? 900009 : 450006 + i);
	for (i = 0; i < ARCHIVE_CLUSTERS - 800000; ++i)
		set_fat(900009 + i, i + 1 == ARCHIVE_CLUSTERS - 800000 ? 0x7fffffff : 900010 + i);
}

static void prepare(FATFS *fs, FIL *file, DWORD *table, DWORD entries)
{
	memset(fs, 0, sizeof(*fs));
	memset(file, 0, sizeof(*file));
	fs->fs_type = FS_EXFAT;
	fs->id = 1;
	fs->csize = CLUSTER_BYTES / 512;
	fs->n_fatent = FAT_ENTRIES;
	fs->fatbase = FAT_BASE;
	fs->fsize = sizeof(fat) / 512;
	fs->database = 20000;
	fs->winsect = (LBA_t)-1;
	file->obj.fs = fs;
	file->obj.id = fs->id;
	file->obj.sclust = 2;
	file->obj.objsize = ARCHIVE_BYTES;
	file->flag = FA_READ;
	file->cltbl = table;
	table[0] = entries;
	elapsed_ms = last_service_ms = service_count = read_count = 0;
	sector_count = write_count = 0;
	allow_window_flush = 0;
	timed_out = 0;
	fail_sector = (LBA_t)-1;
}

static void audit_snapshot(const char *path)
{
	FATFS fs;
	DIR dir;
	FILINFO info;
	FIL file;
	DWORD table[128];
	snapshot = fopen(path, "rb");
	assert(snapshot && fseek(snapshot, 0, SEEK_END) == 0);
	snapshot_size = ftell(snapshot);
	service_enabled = 1;
	fail_sector = (LBA_t)-1;
	assert(f_mount(&fs, "0:", 1) == FR_OK);
	assert(f_opendir(&dir, "0:/") == FR_OK);
	for (;;) {
		assert(f_readdir(&dir, &info) == FR_OK);
		if (!info.fname[0]) break;
		size_t length = strlen(info.fname);
		if (length < 4 || strcmp(info.fname + length - 4, ".zim")) continue;
		assert(f_open(&file, info.fname, FA_READ) == FR_OK);
		file.cltbl = table;
		table[0] = sizeof(table) / sizeof(table[0]);
		read_count = sector_count = 0;
		assert(f_lseek(&file, CREATE_LINKMAP) == FR_OK);
		DWORD total = 0;
		for (unsigned i = 1; i + 1 < table[0]; i += 2) {
			assert(f_lseek(&file, (uint64_t)total * fs.csize * 512 + 512) == FR_OK);
			assert(file.clust == table[i + 1]);
			total += table[i];
		}
		assert(total == (file.obj.objsize + fs.csize * 512 - 1) / (fs.csize * 512));
		printf("%s: stat=%u, %u clusters, %u extents, %u read commands, %u sectors\n",
		       info.fname, file.obj.stat, total, (table[0] - 2) / 2, read_count, sector_count);
		for (unsigned i = 1; i + 1 < table[0]; i += 2)
			printf("  %u %u\n", table[i + 1], table[i]);
		assert(f_close(&file) == FR_OK);
	}
	assert(f_closedir(&dir) == FR_OK);
	assert(f_mount(NULL, "0:", 0) == FR_OK);
	assert(fclose(snapshot) == 0);
	snapshot = NULL;
}

int main(int argc, char **argv)
{
	if (argc == 2) {
		audit_snapshot(argv[1]);
		return 0;
	}
	assert(argc == 1);
	FATFS fs;
	FIL file;
	DWORD table[8];
	const DWORD expected[] = {8, 400000, 2, 400000, 450005,
		ARCHIVE_CLUSTERS - 800000, 900009, 0};
	make_chain();

	/* Model the old firmware: no watchdog service during the walk. */
	prepare(&fs, &file, table, 8);
	service_enabled = 0;
	assert(f_lseek(&file, CREATE_LINKMAP) == FR_DISK_ERR && timed_out);

	/* Both the sizing pass and the full map must survive slow metadata I/O. */
	prepare(&fs, &file, table, 4);
	service_enabled = 1;
	assert(f_lseek(&file, CREATE_LINKMAP) == FR_NOT_ENOUGH_CORE);
	assert(table[0] == 8 && !timed_out && service_count > 0 && elapsed_ms > 20000);
	prepare(&fs, &file, table, 8);
	assert(f_lseek(&file, CREATE_LINKMAP) == FR_OK);
	assert(memcmp(table, expected, sizeof(expected)) == 0);
	assert(!timed_out && service_count > 0 && elapsed_ms > 20000);
#if FF_FASTSEEK_CACHE_SECTORS
	/* Three extents can each leave one partly used read-ahead batch.
	 * Bound both command count and over-read for the configured batch size. */
	unsigned needed_sectors = (ARCHIVE_CLUSTERS + 127) / 128 + 3;
	assert(read_count <= needed_sectors / FF_FASTSEEK_CACHE_SECTORS + 4);
	assert(sector_count <= needed_sectors + 3 * FF_FASTSEEK_CACHE_SECTORS);
#endif
	printf("Mapped %u clusters in %u simulated ms, %u commands/%u sectors without watchdog expiry\n",
	       ARCHIVE_CLUSTERS, elapsed_ms, read_count, sector_count);

	/* Fast seeks cross both extent boundaries well above 4 GiB. */
	unsigned reads = read_count;
	assert(f_lseek(&file, 400000 * CLUSTER_BYTES + 512) == FR_OK);
	assert(file.clust == 450005);
	assert(f_lseek(&file, 800000 * CLUSTER_BYTES + 512) == FR_OK);
	assert(file.clust == 900009 && read_count == reads);

	/* Keep real I/O failures visible. */
	prepare(&fs, &file, table, 8);
	fail_sector = FAT_BASE + 10;
	assert(f_lseek(&file, CREATE_LINKMAP) == FR_DISK_ERR && !timed_out);

	/* The last read-ahead must stop at the FAT boundary. */
	set_fat(FAT_ENTRIES - 2, FAT_ENTRIES - 1);
	set_fat(FAT_ENTRIES - 1, 0x7fffffff);
	prepare(&fs, &file, table, 8);
	file.obj.sclust = FAT_ENTRIES - 2;
	file.obj.objsize = 2 * CLUSTER_BYTES;
	assert(f_lseek(&file, CREATE_LINKMAP) == FR_OK);
	assert(table[0] == 4 && table[1] == 2 && table[2] == FAT_ENTRIES - 2);
	assert(read_count == 1 && sector_count == 1);

	/* Honor a pending FAT edit before reading through the separate cache. */
	prepare(&fs, &file, table, 8);
	file.obj.sclust = FAT_ENTRIES - 2;
	file.obj.objsize = 2 * CLUSTER_BYTES;
	fs.winsect = FAT_BASE + fs.fsize - 1;
	memcpy(fs.win, fat + (fs.fsize - 1) * 512, 512);
	fs.wflag = 1;
	allow_window_flush = 1;
	set_fat(FAT_ENTRIES - 1, 0);
	assert(f_lseek(&file, CREATE_LINKMAP) == FR_OK);
#if FF_FASTSEEK_CACHE_SECTORS
	assert(write_count == 1 && fs.wflag == 0);
#endif
	assert(table[0] == 4 && table[1] == 2);

	/* Contiguous exFAT chains have no usable FAT and need no disk reads. */
	prepare(&fs, &file, table, 8);
	file.obj.stat = 2;
	assert(f_lseek(&file, CREATE_LINKMAP) == FR_OK);
	assert(table[0] == 4 && table[1] == ARCHIVE_CLUSTERS && read_count == 0);

	/* Refreshing the watchdog must not hide a cyclic allocation chain. */
	set_fat(900009 + ARCHIVE_CLUSTERS - 800000 - 1, 2);
	prepare(&fs, &file, table, 8);
	assert(f_lseek(&file, CREATE_LINKMAP) == FR_INT_ERR && !timed_out);
	puts("PASS: seek-map watchdog, batched reads, sizing retry, 64-bit seeks, FAT boundary, dirty window, contiguous chain, I/O failure and cycle bound");
	return 0;
}
