/* Discovery of the ZIM archives present on the card. */
#include "zim_catalog.h"

#include <grifo.h>

#include <stdlib.h>
#include <string.h>

#include "zim_archive.h"
#include "zim_blob.h"
#include "zim_file.h"

#define ZIM_CATALOG_PATH_MAX 200
#define ZIM_CATALOG_TITLE_MAX 48

typedef struct {
	char path[ZIM_CATALOG_PATH_MAX];
	unsigned char title[ZIM_CATALOG_TITLE_MAX];
	uint64_t size;
	int title_ready;
} ZIM_CATALOG_ENTRY;

static ZIM_CATALOG_ENTRY catalog[ZIM_CATALOG_MAX];
static int catalog_count;

/* The two documented layouts: archives in the root of the exFAT content
 * volume, or in zim/ on the FAT32 boot volume for archives under 4 GiB. */
static const char *const catalog_directories[] = { "1:/", "zim/" };

static int has_zim_suffix(const char *name, size_t length)
{
	static const char suffix[] = ".zim";
	size_t i;

	if (length <= 4)
		return 0;
	for (i = 0; i < 4; i++) {
		char c = name[length - 4 + i];
		if (c >= 'A' && c <= 'Z')
			c = (char)(c - 'A' + 'a');
		if (c != suffix[i])
			return 0;
	}
	return 1;
}

static void catalog_add(const char *path, uint64_t size)
{
	int i;
	int at;

	if (catalog_count >= ZIM_CATALOG_MAX)
		return;
	for (at = 0; at < catalog_count; at++)
		if (strcmp(path, catalog[at].path) < 0)
			break;
	for (i = catalog_count; i > at; i--)
		catalog[i] = catalog[i - 1];
	memset(&catalog[at], 0, sizeof(catalog[at]));
	strncpy(catalog[at].path, path, sizeof(catalog[at].path) - 1);
	catalog[at].size = size;
	catalog_count++;
}

static void scan_directory(const char *directory)
{
	char name[ZIM_CATALOG_PATH_MAX];
	char path[ZIM_CATALOG_PATH_MAX];
	size_t prefix = strlen(directory);
	int handle;

	handle = directory_open(directory);
	if (handle < 0)
		return;
	for (;;) {
		ssize_t length = directory_read(handle, name, sizeof(name) - 1);
		uint64_t size = 0;

		if (length <= 0)
			break;
		name[length] = '\0';
		if (name[0] == '.' || !has_zim_suffix(name, (size_t)length) ||
		    prefix + (size_t)length >= sizeof(path))
			continue;
		memcpy(path, directory, prefix);
		memcpy(path + prefix, name, (size_t)length + 1);
		/* Directories and empty files are not archives. */
		if (file_size64(path, &size) != FILE_ERROR_OK || !size)
			continue;
		catalog_add(path, size);
	}
	directory_close(handle);
}

int zim_catalog_scan(void)
{
	unsigned int i;

	catalog_count = 0;
	memset(catalog, 0, sizeof(catalog));
	for (i = 0; i < sizeof(catalog_directories) / sizeof(*catalog_directories); i++)
		scan_directory(catalog_directories[i]);
	return catalog_count;
}

int zim_catalog_count(void)
{
	return catalog_count;
}

const char *zim_catalog_path(int index)
{
	if (index < 0 || index >= catalog_count)
		return NULL;
	return catalog[index].path;
}

uint64_t zim_catalog_size(int index)
{
	if (index < 0 || index >= catalog_count)
		return 0;
	return catalog[index].size;
}

int zim_catalog_find(const char *path)
{
	int i;

	if (!path)
		return -1;
	for (i = 0; i < catalog_count; i++)
		if (!strcmp(catalog[i].path, path))
			return i;
	return -1;
}

/* The file name without directory and suffix, underscores as spaces. */
static void title_from_path(ZIM_CATALOG_ENTRY *entry)
{
	const char *name = strrchr(entry->path, '/');
	size_t length;
	size_t i;

	name = name ? name + 1 : entry->path;
	length = strlen(name) - 4;
	if (length >= sizeof(entry->title))
		length = sizeof(entry->title) - 1;
	for (i = 0; i < length; i++)
		entry->title[i] = name[i] == '_' ? ' ' : (unsigned char)name[i];
	entry->title[length] = '\0';
}

static int device_read_at(void *opaque, uint64_t offset, void *buffer,
			  size_t length)
{
	return zim_file_read_at((ZIM_FILE *)opaque, offset, buffer, length);
}

/* Read the archive's M/Title metadata entry into the catalog entry. */
static void title_from_archive(ZIM_CATALOG_ENTRY *entry)
{
	ZIM_FILE *file;
	ZIM_ARCHIVE *archive;
	ZIM_DIRENT dirent;
	ZIM_IO io;
	size_t size;
	int rc;

	file = memory_allocate(sizeof(*file), "zim-catalog");
	archive = memory_allocate(sizeof(*archive), "zim-catalog");
	if (!file || !archive)
		goto out;
	if (zim_file_open(file, entry->path))
		goto out;
	io.read_at = device_read_at;
	io.opaque = file;
	io.size = file->size;
	if (!zim_archive_open(archive, &io)) {
		rc = zim_archive_find_path(archive, 'M', "Title", &dirent);
		if (!rc || rc == ZIM_ERR_TRUNCATED) {
			unsigned char text[ZIM_CATALOG_TITLE_MAX];

			rc = zim_archive_read_blob(archive, &dirent, text,
						   sizeof(text) - 1, &size);
			if ((!rc || rc == ZIM_ERR_TRUNCATED) && size) {
				if (size > sizeof(text) - 1)
					size = sizeof(text) - 1;
				while (size && (text[size - 1] == '\n' ||
						text[size - 1] == '\r' ||
						text[size - 1] == ' '))
					size--;
				if (size) {
					memcpy(entry->title, text, size);
					entry->title[size] = '\0';
				}
			}
		}
	}
	/* Reading through a second archive disturbs the shared decode cache. */
	zim_blob_cache_reset();
	zim_file_close(file);
out:
	if (file)
		memory_free(file, "zim-catalog");
	if (archive)
		memory_free(archive, "zim-catalog");
}

const unsigned char *zim_catalog_title(int index)
{
	ZIM_CATALOG_ENTRY *entry;

	if (index < 0 || index >= catalog_count)
		return (const unsigned char *)"";
	entry = &catalog[index];
	if (!entry->title_ready) {
		title_from_path(entry);
		title_from_archive(entry);
		entry->title_ready = 1;
	}
	return entry->title;
}
