/* FatFs R0.16 disk interface adapted to the existing WikiReader MMC driver. */
#ifndef WIKIREADER_FATFS_R016_DISKIO_H
#define WIKIREADER_FATFS_R016_DISKIO_H

#include "ff.h"

typedef BYTE DSTATUS;

typedef enum {
	RES_OK = 0,
	RES_ERROR,
	RES_WRPRT,
	RES_NOTRDY,
	RES_PARERR
} DRESULT;

/* Keep these declarations independent of the legacy integer.h/diskio.h,
 * whose typedefs predate and conflict with R0.16's stdint-based ff.h. */
DSTATUS mmc_disk_initialize(BYTE drive);
DSTATUS mmc_disk_status(BYTE drive);
DRESULT mmc_disk_read(BYTE drive, BYTE *buffer, DWORD sector, BYTE count);
DRESULT mmc_disk_write(BYTE drive, const BYTE *buffer, DWORD sector,
		       BYTE count);
DRESULT mmc_disk_ioctl(BYTE drive, BYTE command, void *buffer);

static inline DSTATUS disk_initialize(BYTE drive)
{
	return mmc_disk_initialize(drive);
}

static inline DSTATUS disk_status(BYTE drive)
{
	return mmc_disk_status(drive);
}

static inline DRESULT disk_read(BYTE drive, BYTE *buffer, LBA_t sector,
				UINT count)
{
	if (!count || count > 255)
		return RES_PARERR;
	return mmc_disk_read(drive, buffer, (DWORD)sector, (BYTE)count);
}

static inline DRESULT disk_write(BYTE drive, const BYTE *buffer,
				 LBA_t sector, UINT count)
{
	if (!count || count > 255)
		return RES_PARERR;
	return mmc_disk_write(drive, buffer, (DWORD)sector, (BYTE)count);
}

static inline DRESULT disk_ioctl(BYTE drive, BYTE command, void *buffer)
{
	return mmc_disk_ioctl(drive, command, buffer);
}

#define STA_NOINIT  0x01
#define STA_NODISK  0x02
#define STA_PROTECT 0x04

/* These values are the established ABI used by the WikiReader MMC driver.
 * TRIM is disabled in ffconf.h, so command 4 remains the board's power
 * control rather than the newer FatFs CTRL_TRIM assignment. */
#define CTRL_SYNC        0
#define GET_SECTOR_COUNT 1
#define GET_SECTOR_SIZE  2
#define GET_BLOCK_SIZE   3
#define CTRL_POWER       4
#define CTRL_LOCK        5
#define CTRL_EJECT       6

#define MMC_GET_TYPE   10
#define MMC_GET_CSD    11
#define MMC_GET_CID    12
#define MMC_GET_OCR    13
#define MMC_GET_SDSTAT 14

#endif
