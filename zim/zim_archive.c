/*
 * Minimal ZIM 6 reader shared by the host diagnostics and WikiReader app.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "zim_archive.h"

#if defined(__c33__) && defined(ZIM_BENCH)
#include <grifo.h>
#include "zim_bench.h"
/* The card reads of the index itself, above all the directory entry that
 * starts every article load.  These were outside the benchmark's card
 * slot, so a card powering back up after an idle pause showed only as
 * unattributed time in the blob phase. */
#define BENCH_TIMED(slot, bytes, statement) do { \
		unsigned long bench_t0_ = timer_get(); \
		statement; \
		zim_bench_account(slot, timer_get() - bench_t0_, bytes); \
	} while (0)
#else
#define BENCH_TIMED(slot, bytes, statement) do { statement; } while (0)
#endif

#include <string.h>

#define ZIM_MAGIC 0x044d495aUL
#define ZIM_HEADER_SIZE 80
#define ZIM_REDIRECT_MIME 0xffff
#define ZIM_TITLE_LISTING_PATH "listing/titleOrdered/v1"

static uint16_t get_le16(const unsigned char *p)
{
	return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}

static uint32_t get_le32(const unsigned char *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
		(uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t get_le64(const unsigned char *p)
{
	return (uint64_t)get_le32(p) | (uint64_t)get_le32(p + 4) << 32;
}

static int range_valid(const ZIM_ARCHIVE *archive, uint64_t offset,
		       uint64_t length)
{
	return offset <= archive->io.size && length <= archive->io.size - offset;
}

static int read_exact(const ZIM_ARCHIVE *archive, uint64_t offset,
		      void *buffer, size_t length)
{
	int rc;

	if (!range_valid(archive, offset, length))
		return ZIM_ERR_RANGE;
	BENCH_TIMED(ZIM_BENCH_SLOT_CARD, length,
		    rc = archive->io.read_at(archive->io.opaque, offset, buffer,
					     length));
	return rc ? ZIM_ERR_IO : ZIM_OK;
}

static int read_u32(const ZIM_ARCHIVE *archive, uint64_t offset,
		    uint32_t *value)
{
	unsigned char bytes[4];
	int rc = read_exact(archive, offset, bytes, sizeof(bytes));
	if (!rc)
		*value = get_le32(bytes);
	return rc;
}

static int read_u64(const ZIM_ARCHIVE *archive, uint64_t offset,
		    uint64_t *value)
{
	unsigned char bytes[8];
	int rc = read_exact(archive, offset, bytes, sizeof(bytes));
	if (!rc)
		*value = get_le64(bytes);
	return rc;
}

static int read_cstring(const ZIM_ARCHIVE *archive, uint64_t *offset,
			char *text, size_t capacity)
{
	unsigned char chunk[64];
	size_t used = 0;
	int truncated = 0;

	if (!capacity)
		return ZIM_ERR_RANGE;
	for (;;) {
		size_t amount;
		size_t i;
		int rc;

		if (*offset >= archive->io.size)
			return ZIM_ERR_RANGE;
		amount = sizeof(chunk);
		if ((uint64_t)amount > archive->io.size - *offset)
			amount = (size_t)(archive->io.size - *offset);
		rc = read_exact(archive, *offset, chunk, amount);
		if (rc)
			return rc;
		for (i = 0; i < amount; i++) {
			if (!chunk[i]) {
				*offset += i + 1;
				text[used] = '\0';
				return truncated ? ZIM_ERR_TRUNCATED : ZIM_OK;
			}
			if (used + 1 < capacity)
				text[used++] = (char)chunk[i];
			else
				truncated = 1;
		}
		*offset += amount;
	}
}

static int compare_dirent_path(char name_space, const char *path,
			       const ZIM_DIRENT *dirent)
{
	if (name_space < dirent->name_space)
		return -1;
	if (name_space > dirent->name_space)
		return 1;
	return strcmp(path, dirent->path);
}

static int prefix_compare(const char *prefix, const char *title)
{
	while (*prefix && *title && *prefix == *title) {
		prefix++;
		title++;
	}
	if (!*prefix)
		return 0;
	return (unsigned char)*prefix < (unsigned char)*title ? -1 : 1;
}

static int locate_uncompressed_blob(const ZIM_ARCHIVE *archive,
				    const ZIM_DIRENT *dirent,
				    uint64_t *blob_pos,
				    uint64_t *blob_size)
{
	uint64_t cluster_start;
	uint64_t cluster_end;
	uint64_t first_blob;
	uint64_t next_blob;
	uint64_t offset_size;
	uint8_t compression;
	uint8_t extended;
	int rc;

	rc = zim_archive_blob_location(archive, dirent, &cluster_start,
				       &cluster_end, &compression, &extended);
	if (rc)
		return rc;
	if (compression != 1)
		return ZIM_ERR_UNSUPPORTED;
	offset_size = extended ? 8 : 4;
	if (extended) {
		rc = read_u64(archive, cluster_start + 1 +
			      (uint64_t)dirent->blob_number * 8, &first_blob);
		if (!rc)
			rc = read_u64(archive, cluster_start + 1 +
				      ((uint64_t)dirent->blob_number + 1) * 8,
				      &next_blob);
	} else {
		uint32_t first32 = 0;
		uint32_t next32 = 0;
		rc = read_u32(archive, cluster_start + 1 +
			      (uint64_t)dirent->blob_number * 4, &first32);
		if (!rc)
			rc = read_u32(archive, cluster_start + 1 +
				      ((uint64_t)dirent->blob_number + 1) * 4,
				      &next32);
		first_blob = first32;
		next_blob = next32;
	}
	if (rc)
		return rc;
	if (first_blob < offset_size || next_blob < first_blob ||
	    next_blob > cluster_end - cluster_start - 1)
		return ZIM_ERR_FORMAT;
	*blob_pos = cluster_start + 1 + first_blob;
	*blob_size = next_blob - first_blob;
	return ZIM_OK;
}

int zim_archive_open(ZIM_ARCHIVE *archive, const ZIM_IO *io)
{
	unsigned char header[ZIM_HEADER_SIZE];
	ZIM_DIRENT listing;
	uint64_t listing_size;
	int rc;

	if (!archive || !io || !io->read_at)
		return ZIM_ERR_RANGE;
	memset(archive, 0, sizeof(*archive));
	archive->io = *io;
	if (io->size < sizeof(header))
		return ZIM_ERR_FORMAT;
	rc = read_exact(archive, 0, header, sizeof(header));
	if (rc)
		return rc;
	if (get_le32(header) != ZIM_MAGIC)
		return ZIM_ERR_FORMAT;
	archive->major_version = get_le16(header + 4);
	archive->minor_version = get_le16(header + 6);
	if (archive->major_version != 6)
		return ZIM_ERR_UNSUPPORTED;
	archive->entry_count = get_le32(header + 24);
	archive->cluster_count = get_le32(header + 28);
	archive->path_ptr_pos = get_le64(header + 32);
	archive->title_ptr_pos = get_le64(header + 40);
	archive->cluster_ptr_pos = get_le64(header + 48);
	archive->mime_list_pos = get_le64(header + 56);
	archive->main_page = get_le32(header + 64);
	archive->checksum_pos = get_le64(header + 72);
	if (!archive->entry_count || !archive->cluster_count ||
	    archive->cluster_count > archive->entry_count ||
	    archive->mime_list_pos != ZIM_HEADER_SIZE ||
	    !range_valid(archive, archive->path_ptr_pos,
			 (uint64_t)archive->entry_count * 8) ||
	    !range_valid(archive, archive->cluster_ptr_pos,
			 (uint64_t)archive->cluster_count * 8))
		return ZIM_ERR_FORMAT;

	/* New archives keep the front-article title listing in this
	 * well-known uncompressed item. Older v6 archives use title_ptr_pos. */
	rc = zim_archive_find_path(archive, 'X', ZIM_TITLE_LISTING_PATH,
				   &listing);
	if (!rc) {
		rc = locate_uncompressed_blob(archive, &listing,
					      &archive->title_listing_pos,
					      &listing_size);
		if (rc)
			return rc;
		if (listing_size % 4 || listing_size / 4 > 0xffffffffULL)
			return ZIM_ERR_FORMAT;
		archive->title_listing_count = (uint32_t)(listing_size / 4);
	} else if (archive->title_ptr_pos != 0xffffffffffffffffULL &&
		   range_valid(archive, archive->title_ptr_pos,
			       (uint64_t)archive->entry_count * 4)) {
		archive->title_listing_pos = archive->title_ptr_pos;
		archive->title_listing_count = archive->entry_count;
	} else {
		return rc;
	}
	return ZIM_OK;
}

int zim_archive_read_dirent(const ZIM_ARCHIVE *archive, uint32_t path_index,
			    ZIM_DIRENT *dirent)
{
	unsigned char header[16];
	uint64_t offset;
	uint64_t text_offset;
	int rc;
	int path_rc;
	int title_rc;

	if (!archive || !dirent || path_index >= archive->entry_count)
		return ZIM_ERR_RANGE;
	rc = read_u64(archive, archive->path_ptr_pos + (uint64_t)path_index * 8,
		      &offset);
	if (rc)
		return rc;
	if (!range_valid(archive, offset, 16))
		return ZIM_ERR_FORMAT;
	memset(dirent, 0, sizeof(*dirent));
	dirent->path_index = path_index;
	rc = read_exact(archive, offset, header, sizeof(header));
	if (rc)
		return rc;
	dirent->mime_type = get_le16(header);
	dirent->parameter_length = header[2];
	dirent->name_space = (char)header[3];
	dirent->revision = get_le32(header + 4);
	if (dirent->mime_type == ZIM_REDIRECT_MIME) {
		dirent->redirect_index = get_le32(header + 8);
		text_offset = offset + 12;
	} else {
		dirent->cluster_number = get_le32(header + 8);
		dirent->blob_number = get_le32(header + 12);
		text_offset = offset + 16;
	}
	path_rc = read_cstring(archive, &text_offset, dirent->path,
			       sizeof(dirent->path));
	if (path_rc && path_rc != ZIM_ERR_TRUNCATED)
		return path_rc;
	title_rc = read_cstring(archive, &text_offset, dirent->title,
				sizeof(dirent->title));
	if (title_rc && title_rc != ZIM_ERR_TRUNCATED)
		return title_rc;
	if (!dirent->title[0]) {
		strncpy(dirent->title, dirent->path, sizeof(dirent->title) - 1);
		dirent->title[sizeof(dirent->title) - 1] = '\0';
	}
	return path_rc || title_rc ? ZIM_ERR_TRUNCATED : ZIM_OK;
}

int zim_archive_find_path(const ZIM_ARCHIVE *archive, char name_space,
			  const char *path, ZIM_DIRENT *dirent)
{
	uint32_t lower = 0;
	uint32_t upper;
	ZIM_DIRENT current;
	int rc;

	if (!archive || !path)
		return ZIM_ERR_RANGE;
	upper = archive->entry_count;
	while (lower < upper) {
		uint32_t middle = lower + (upper - lower) / 2;
		rc = zim_archive_read_dirent(archive, middle, &current);
		if (rc && rc != ZIM_ERR_TRUNCATED)
			return rc;
		rc = compare_dirent_path(name_space, path, &current);
		if (rc <= 0)
			upper = middle;
		else
			lower = middle + 1;
	}
	if (lower >= archive->entry_count)
		return ZIM_ERR_NOT_FOUND;
	rc = zim_archive_read_dirent(archive, lower, &current);
	if (rc && rc != ZIM_ERR_TRUNCATED)
		return rc;
	if (compare_dirent_path(name_space, path, &current))
		return ZIM_ERR_NOT_FOUND;
	if (dirent)
		*dirent = current;
	return rc;
}

int zim_archive_title_at(const ZIM_ARCHIVE *archive, uint32_t title_index,
			 ZIM_DIRENT *dirent)
{
	uint32_t path_index;
	int rc;

	if (!archive || title_index >= archive->title_listing_count)
		return ZIM_ERR_RANGE;
	rc = read_u32(archive,
		      archive->title_listing_pos + (uint64_t)title_index * 4,
		      &path_index);
	if (rc)
		return rc;
	return zim_archive_read_dirent(archive, path_index, dirent);
}

int zim_archive_find_title_prefix(const ZIM_ARCHIVE *archive,
				  const char *prefix,
				  uint32_t *first_title_index)
{
	uint32_t lower = 0;
	uint32_t upper;
	ZIM_DIRENT dirent;
	int rc;

	if (!archive || !prefix || !*prefix || !first_title_index)
		return ZIM_ERR_RANGE;
	upper = archive->title_listing_count;
	while (lower < upper) {
		uint32_t middle = lower + (upper - lower) / 2;
		rc = zim_archive_title_at(archive, middle, &dirent);
		if (rc && rc != ZIM_ERR_TRUNCATED)
			return rc;
		if (strcmp(dirent.title, prefix) < 0)
			lower = middle + 1;
		else
			upper = middle;
	}
	if (lower >= archive->title_listing_count)
		return ZIM_ERR_NOT_FOUND;
	rc = zim_archive_title_at(archive, lower, &dirent);
	if (rc && rc != ZIM_ERR_TRUNCATED)
		return rc;
	if (prefix_compare(prefix, dirent.title))
		return ZIM_ERR_NOT_FOUND;
	*first_title_index = lower;
	return ZIM_OK;
}

int zim_archive_blob_location(const ZIM_ARCHIVE *archive,
			      const ZIM_DIRENT *dirent,
			      uint64_t *cluster_start,
			      uint64_t *cluster_end,
			      uint8_t *compression,
			      uint8_t *extended)
{
	unsigned char info;
	int rc;

	if (!archive || !dirent || !cluster_start || !cluster_end ||
	    !compression || !extended ||
	    dirent->mime_type == ZIM_REDIRECT_MIME ||
	    dirent->cluster_number >= archive->cluster_count)
		return ZIM_ERR_RANGE;
	rc = read_u64(archive, archive->cluster_ptr_pos +
		      (uint64_t)dirent->cluster_number * 8, cluster_start);
	if (rc)
		return rc;
	if (dirent->cluster_number + 1 < archive->cluster_count)
		rc = read_u64(archive, archive->cluster_ptr_pos +
			      ((uint64_t)dirent->cluster_number + 1) * 8,
			      cluster_end);
	else {
		*cluster_end = archive->checksum_pos;
		if (!*cluster_end || *cluster_end > archive->io.size)
			*cluster_end = archive->io.size;
	}
	if (rc)
		return rc;
	if (*cluster_start >= *cluster_end ||
	    !range_valid(archive, *cluster_start, *cluster_end - *cluster_start))
		return ZIM_ERR_FORMAT;
	rc = read_exact(archive, *cluster_start, &info, 1);
	if (rc)
		return rc;
	*compression = info & 0x0f;
	if (!*compression)
		*compression = 1;
	*extended = !!(info & 0x10);
	if (*compression != 1 && *compression != 4 && *compression != 5)
		return ZIM_ERR_UNSUPPORTED;
	return ZIM_OK;
}

const char *zim_error_string(int error)
{
	switch (error) {
	case ZIM_OK: return "success";
	case ZIM_ERR_IO: return "I/O error";
	case ZIM_ERR_FORMAT: return "invalid ZIM structure";
	case ZIM_ERR_RANGE: return "offset or argument out of range";
	case ZIM_ERR_UNSUPPORTED: return "unsupported ZIM feature";
	case ZIM_ERR_NOT_FOUND: return "entry not found";
	case ZIM_ERR_TRUNCATED: return "path or title was truncated";
	default: return "unknown error";
	}
}
