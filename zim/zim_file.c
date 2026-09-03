/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "zim_file.h"

#include <grifo.h>

#include <stdlib.h>
#include <string.h>

static int read_exact(ZIM_FILE *file, uint32_t offset, void *buffer,
		      size_t length)
{
	if (file_lseek(file->handle, offset) != FILE_ERROR_OK)
		return -1;
	return file_read(file->handle, buffer, length) == (ssize_t)length ?
		0 : -1;
}

static int create_link_map(ZIM_FILE *file)
{
	unsigned long entries = 4;
	file_error_t result;

	file->link_map = malloc(entries * sizeof(*file->link_map));
	if (!file->link_map)
		return -1;
	result = file_fastseek(file->handle, file->link_map, entries);
	if (result == FILE_ERROR_NOT_ENOUGH_CORE) {
		entries = file->link_map[0];
		if (entries < 4) {
			free(file->link_map);
			file->link_map = NULL;
			return -1;
		}
		free(file->link_map);
		file->link_map = malloc(entries * sizeof(*file->link_map));
		if (!file->link_map)
			return -1;
		result = file_fastseek(file->handle, file->link_map, entries);
	}
	return result == FILE_ERROR_OK ? 0 : -1;
}

int zim_file_open(ZIM_FILE *file, const char *path)
{
	unsigned long size;
	int handle;

	if (!file || !path)
		return -1;
	memset(file, 0, sizeof(*file));
	if (file_size(path, &size) != FILE_ERROR_OK || !size)
		return -1;
	handle = file_open(path, FILE_OPEN_READ);
	if (handle < 0)
		return -1;
	file->handle = handle;
	file->size = (uint32_t)size;
	file->open = 1;
	if (create_link_map(file)) {
		zim_file_close(file);
		return -1;
	}
	return 0;
}

static int read_cached_page(ZIM_FILE *file, uint32_t page,
			    const unsigned char **data)
{
	unsigned int i;
	unsigned int slot;
	uint32_t offset = page * ZIM_FILE_PAGE_SIZE;
	size_t length = ZIM_FILE_PAGE_SIZE;

	for (i = 0; i < ZIM_FILE_CACHE_PAGES; i++) {
		if (file->cache_valid[i] && file->cached_page[i] == page) {
			*data = file->cache[i];
			return 0;
		}
	}
	if (length > file->size - offset)
		length = file->size - offset;
	slot = file->next_cache;
	if (read_exact(file, offset, file->cache[slot], length))
		return -1;
	file->cached_page[slot] = page;
	file->cache_valid[slot] = 1;
	file->next_cache = (unsigned char)((slot + 1) % ZIM_FILE_CACHE_PAGES);
	*data = file->cache[slot];
	return 0;
}

int zim_file_read_at(ZIM_FILE *file, uint64_t offset, void *destination,
		     size_t length)
{
	unsigned char *output = destination;

	if (!file || !file->open || (!destination && length) ||
	    offset > file->size || (uint64_t)length > file->size - offset)
		return -1;
	while (length) {
		uint32_t page_offset = (uint32_t)offset % ZIM_FILE_PAGE_SIZE;
		size_t amount;

		if (!page_offset && length >= ZIM_FILE_PAGE_SIZE) {
			amount = length & ~(size_t)(ZIM_FILE_PAGE_SIZE - 1);
			if (read_exact(file, (uint32_t)offset, output, amount))
				return -1;
		} else {
			const unsigned char *cached;
			uint32_t page = (uint32_t)offset / ZIM_FILE_PAGE_SIZE;

			if (read_cached_page(file, page, &cached))
				return -1;
			amount = ZIM_FILE_PAGE_SIZE - page_offset;
			if (amount > length)
				amount = length;
			memcpy(output, cached + page_offset, amount);
		}
		offset += amount;
		output += amount;
		length -= amount;
	}
	return 0;
}

void zim_file_close(ZIM_FILE *file)
{
	if (!file)
		return;
	if (file->open)
		file_close(file->handle);
	free(file->link_map);
	memset(file, 0, sizeof(*file));
}
