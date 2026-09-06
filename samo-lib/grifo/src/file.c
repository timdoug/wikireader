/*
 * file - file system access
 *
 * Copyright (c) 2009 Openmoko Inc.
 *
 * Authors   Christopher Hall <hsw@openmoko.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "standard.h"

#include <string.h>

#include <ff.h>
#include <diskio.h>

#include "file.h"
#include "power_log.h"
#include "timer.h"


// a type that can hold the path to the file
typedef char FilenameType[128 + 1];

// this determines the maximum files that can be open simultaneously
typedef struct {
	bool IsOpen;
	FIL file;
} FileType;

static FileType FileControlBlock[512];

// this determines the maximum directoriess that can be open simultaneously
typedef struct {
	bool IsOpen;
	DIR directory;
} DirectoryType;

static DirectoryType DirectoryControlBlock[64];

// logical volume 0 is the boot FAT32 partition; volume 1 is exFAT content
static FATFS TheFileSystem[2];

PARTITION VolToPart[FF_VOLUMES] = {
	{ 0, 0 },
	{ 0, 2 },
};


static File_ErrorType FatResult(FRESULT result)
{
	switch (result) {
	case FR_OK:
		return FILE_ERROR_OK;
	case FR_NOT_READY:
		return FILE_ERROR_NOT_READY;
	case FR_NO_FILE:
		return FILE_ERROR_NO_FILE;
	case FR_NO_PATH:
		return FILE_ERROR_NO_PATH;
	case FR_INVALID_NAME:
		return FILE_ERROR_INVALID_NAME;
	case FR_INVALID_DRIVE:
		return FILE_ERROR_INVALID_DRIVE;
	case FR_DENIED:
		return FILE_ERROR_DENIED;
	case FR_EXIST:
		return FILE_ERROR_EXIST;
	case FR_WRITE_PROTECTED:
		return FILE_ERROR_WRITE_PROTECTED;
	case FR_NOT_ENABLED:
		return FILE_ERROR_NOT_ENABLED;
	case FR_NO_FILESYSTEM:
		return FILE_ERROR_NO_FILESYSTEM;
	case FR_INVALID_OBJECT:
		return FILE_ERROR_INVALID_OBJECT;
	case FR_NOT_ENOUGH_CORE:
		return FILE_ERROR_NOT_ENOUGH_CORE;
	case FR_DISK_ERR:
	case FR_INT_ERR:
	default:
		return FILE_ERROR_RW_ERROR;
	}
}


static File_ErrorType DiskResult(DRESULT result)
{
	switch (result) {
	case RES_OK:
		return FILE_ERROR_OK;
	case RES_WRPRT:
		return FILE_ERROR_WRITE_PROTECTED;
	case RES_NOTRDY:
		return FILE_ERROR_NOT_READY;
	case RES_ERROR:
	case RES_PARERR:
	default:
		return FILE_ERROR_RW_ERROR;
	}
}


void File_initialise(void)
{
	size_t i = 0;

	for (i = 0; i < SizeOfArray(FileControlBlock); i++) {
		FileControlBlock[i].IsOpen = false;
	}
	for (i = 0; i < SizeOfArray(DirectoryControlBlock); i++) {
		DirectoryControlBlock[i].IsOpen = false;
	}
	memset(TheFileSystem, 0, sizeof(TheFileSystem));
	{
		uint8_t b = 0;
		disk_ioctl(0, CTRL_POWER, &b);
		disk_initialize(0);
	}
	f_mount(&TheFileSystem[0], "0:", 1);
	/* A one-partition legacy card remains valid; volume 1 can be absent. */
	f_mount(&TheFileSystem[1], "1:", 1);
}


void File_PowerDown(void)
{
	uint8_t b = 0;
	disk_ioctl(0, CTRL_POWER, &b);
}


static void AutoPowerUp(void)
{
	uint8_t b[2] = {2, 0};
	disk_ioctl(0, CTRL_POWER, &b);
	if (0 == b[1]) {
		bool log = PowerLog_enabled();
		unsigned long start = log ? Timer_get() : 0;
		DSTATUS status = disk_initialize(0);
		if (log)
			PowerLog_card_init(Timer_get() - start, !(status & STA_NOINIT));
	}
}


// ensure: 0 <= handle < array size
static FileType *ValidateFileHandle(int handle)
{
	if (0 > handle || SizeOfArray(FileControlBlock) <= handle) {
		return NULL;
	}
	if (!FileControlBlock[handle].IsOpen) {
		return NULL;
	}
	return &FileControlBlock[handle];
}


// ensure: 1 <= handle <= array size
static DirectoryType *ValidateDirectoryHandle(int handle)
{
	if (0 > handle || SizeOfArray(DirectoryControlBlock) <= handle) {
		return NULL;
	}
	if (!DirectoryControlBlock[handle].IsOpen) {
		return NULL;
	}
	return &DirectoryControlBlock[handle];
}


void File_CloseAll(void)
{
	size_t i = 0;

	AutoPowerUp();

	for (i = 0; i < SizeOfArray(FileControlBlock); i++) {
		File_close(i);  // handles are 0-based; i+1 left handle 0 unflushed
	}
	for (i = 0; i < SizeOfArray(DirectoryControlBlock); i++) {
		File_CloseDirectory(i);
	}
	File_initialise();
}


File_ErrorType File_rename(const char *OldFilename, const char *NewFilename)
{
	if (NULL == OldFilename || NULL == NewFilename) {
		return FILE_ERROR_INVALID_NAME;
	}
	AutoPowerUp();
	return FatResult(f_rename(OldFilename, NewFilename));
}

File_ErrorType File_delete(const char *filename)
{
	if (NULL == filename) {
		return FILE_ERROR_INVALID_NAME;
	}
	AutoPowerUp();
	return FatResult(f_unlink(filename));
}


File_ErrorType File_size(const char *filename, unsigned long *length)
{
	if (NULL == filename) {
		return FILE_ERROR_INVALID_NAME;
	}
	FILINFO stat;

	AutoPowerUp();
	File_ErrorType rc = FatResult(f_stat(filename, &stat));
	if (FILE_ERROR_OK == rc) {
		*length = stat.fsize;  // stat is not filled on failure
	}
	return rc;
}


File_ErrorType File_size64(const char *filename, uint64_t *length)
{
	FILINFO stat;
	File_ErrorType rc;

	if (NULL == filename || NULL == length)
		return FILE_ERROR_INVALID_NAME;
	AutoPowerUp();
	rc = FatResult(f_stat(filename, &stat));
	if (FILE_ERROR_OK == rc)
		*length = stat.fsize;
	return rc;
}


File_ErrorType File_create(const char *filename, File_AccessType fam)
{
	// FILE_OPEN_TRUNCATE alone maps to FA_CREATE_ALWAYS, which creates or
	// truncates.  Adding FILE_OPEN_CREATE (FA_CREATE_NEW) made f_open fail
	// with FR_EXIST whenever the file already existed, because FatFs
	// checks the CREATE_NEW bit first.
	return File_open(filename, fam | FILE_OPEN_TRUNCATE);
}


File_ErrorType File_open(const char *filename, File_AccessType fam)
{
	File_ErrorType result;

	if (NULL == filename) {
		return FILE_ERROR_INVALID_NAME;
	}

	size_t i = 0;
	for (i = 0; i < SizeOfArray(FileControlBlock); i++) {
		if (!FileControlBlock[i].IsOpen) {
			AutoPowerUp();
			result = FatResult(f_open(&FileControlBlock[i].file,
						  filename, (BYTE)fam));
			if (FILE_ERROR_OK == result) {
				FileControlBlock[i].IsOpen = true;
				return i;  // handle 0...
			}
			return result;
		}
	}
	return FILE_ERROR_DENIED;
}


File_ErrorType File_close(int handle)
{
	FileType *file = ValidateFileHandle(handle);

	if (NULL == file) {
		return FILE_ERROR_INVALID_OBJECT;
	}
	AutoPowerUp();
	File_ErrorType result = FatResult(f_close(&file->file));
	if (result == FILE_ERROR_OK)
		file->IsOpen = false;
	return result;
}


ssize_t File_read(int handle, void *buffer, size_t length)
{
	FileType *file = ValidateFileHandle(handle);

	if (NULL == file) {
		return FILE_ERROR_INVALID_OBJECT;
	}

	AutoPowerUp();
	unsigned int count;
	File_ErrorType rc = FatResult(f_read(&file->file, buffer, length,
					       &count));
	if (FILE_ERROR_OK == rc) {
		return count;
	}
	return rc;
}


ssize_t File_write(int handle, void *buffer, size_t length)
{
	FileType *file = ValidateFileHandle(handle);

	if (NULL == file) {
		return FILE_ERROR_INVALID_OBJECT;
	}

	AutoPowerUp();
	unsigned int count;
	File_ErrorType rc = FatResult(f_write(&file->file, buffer, length,
						&count));
	if (FILE_ERROR_OK == rc) {
		return count;
	}
	return rc;
}


File_ErrorType File_sync(int handle)
{
	FileType *file = ValidateFileHandle(handle);

	if (NULL == file) {
		return FILE_ERROR_INVALID_OBJECT;
	}

	AutoPowerUp();
	return FatResult(f_sync(&file->file));
}


File_ErrorType File_lseek(int handle, unsigned long pos)
{
	FileType *file = ValidateFileHandle(handle);

	if (NULL == file) {
		return FILE_ERROR_INVALID_OBJECT;
	}

	AutoPowerUp();
	return FatResult(f_lseek(&file->file, pos));
}


File_ErrorType File_lseek64(int handle, uint64_t pos)
{
	FileType *file = ValidateFileHandle(handle);

	if (NULL == file)
		return FILE_ERROR_INVALID_OBJECT;
	AutoPowerUp();
	return FatResult(f_lseek(&file->file, pos));
}


File_ErrorType File_fastseek(int handle, unsigned long *table,
			     unsigned long entries)
{
	FileType *file = ValidateFileHandle(handle);
	File_ErrorType result;

	if (NULL == file)
		return FILE_ERROR_INVALID_OBJECT;
	if (NULL == table)
		return FILE_ERROR_INVALID_NAME;
	if (entries < 4) {
		table[0] = 4;
		return FILE_ERROR_NOT_ENOUGH_CORE;
	}

	AutoPowerUp();
	/* exFAT records an unfragmented file as a contiguous extent.  FatFs'
	 * generic CREATE_LINKMAP path still visits every cluster even though
	 * get_fat() merely synthesizes the next cluster for this status. */
	if (file->file.obj.fs->fs_type == FS_EXFAT &&
	    file->file.obj.stat == 2 && file->file.obj.sclust != 0) {
		uint64_t cluster_bytes =
			(uint64_t)file->file.obj.fs->csize * FF_MAX_SS;
		uint64_t cluster_count =
			(file->file.obj.objsize + cluster_bytes - 1) /
			cluster_bytes;

		table[0] = 4;
		table[1] = (DWORD)cluster_count;
		table[2] = file->file.obj.sclust;
		table[3] = 0;
		file->file.cltbl = (DWORD *)table;
		return FILE_ERROR_OK;
	}
	table[0] = entries;
	file->file.cltbl = (DWORD *)table;
	result = FatResult(f_lseek(&file->file, CREATE_LINKMAP));
	if (result != FILE_ERROR_OK)
		file->file.cltbl = NULL;
	return result;
}


//File_ErrorType File_ltell(int handle, unsigned long *pos); // not available yet


File_ErrorType File_CreateDirectory(const char *directoryname)
{
	if (NULL == directoryname) {
		return FILE_ERROR_INVALID_NAME;
	}

	AutoPowerUp();
	return FatResult(f_mkdir(directoryname));
}


bool File_DirectoryExists(const char *directoryname)
{
	DIR dir;
	if (NULL == directoryname) {
		return false;
	}

	AutoPowerUp();
	File_ErrorType rc = FatResult(f_opendir(&dir, directoryname));
	if (FILE_ERROR_OK == rc) {
		f_closedir(&dir);
		return true;
	}

	return false;
}


File_ErrorType File_OpenDirectory(const char *directoryname)
{
	File_ErrorType result;

	if (NULL == directoryname) {
		return FILE_ERROR_INVALID_NAME;
	}

	size_t i;
	for (i = 0; i < SizeOfArray(DirectoryControlBlock); i++) {
		if (!DirectoryControlBlock[i].IsOpen) {
			AutoPowerUp();
			result = FatResult(f_opendir(
				&DirectoryControlBlock[i].directory, directoryname));
			if (FILE_ERROR_OK == result) {
				DirectoryControlBlock[i].IsOpen = true;
				return i; // 0...
			}
			return result;
		}
	}
	return FILE_ERROR_DENIED;
}


File_ErrorType File_CloseDirectory(int handle)
{
	DirectoryType *directory = ValidateDirectoryHandle(handle);

	if (NULL == directory) {
		return FILE_ERROR_INVALID_OBJECT;
	}

	File_ErrorType result = FatResult(f_closedir(&directory->directory));
	if (result == FILE_ERROR_OK)
		directory->IsOpen = false;
	return result;
}


ssize_t File_ReadDirectory(int handle, void *buffer, size_t length)
{
	DirectoryType *directory = ValidateDirectoryHandle(handle);

	if (NULL == directory) {
		return FILE_ERROR_INVALID_OBJECT;
	}

	FILINFO info;

	AutoPowerUp();
	File_ErrorType rc = FatResult(f_readdir(&directory->directory, &info));
	if (FILE_ERROR_OK == rc){
		size_t count = strlen(info.fname);

		if (count > length) {
			count = length;
		}
		if (0 != count) {
			memcpy(buffer, info.fname, count);
		}
		return count;
	}
	return rc;
}


File_ErrorType File_AbsoluteRead(unsigned long sector, void *buffer, int count)
{
	AutoPowerUp();
	return DiskResult(disk_read(0, buffer, sector, (UINT)count));
}


File_ErrorType File_AbsoluteWrite(unsigned long sector, const void *buffer, int count)
{
	AutoPowerUp();
	return DiskResult(disk_write(0, buffer, sector, (UINT)count));
}
