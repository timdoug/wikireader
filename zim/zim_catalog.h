/* Discovery of the ZIM archives present on the card. */
#ifndef WIKIREADER_ZIM_CATALOG_H
#define WIKIREADER_ZIM_CATALOG_H

#include <inttypes.h>

/* Archive ids occupy four bits above the article index (lcd_buf_draw.h). */
#define ZIM_CATALOG_MAX 15

/* Scan the exFAT content volume root and the boot volume's zim/ directory
 * for *.zim files.  Entries are sorted by path.  Returns the count. */
int zim_catalog_scan(void);
int zim_catalog_count(void);

/* Path suitable for file_open(), or NULL when index is out of range. */
const char *zim_catalog_path(int index);

/* Archive size in bytes, 0 when unknown. */
uint64_t zim_catalog_size(int index);

/* The archive's own M/Title, read on first use; falls back to the file name.
 * The result points at storage owned by the catalog. */
const unsigned char *zim_catalog_title(int index);

/* Index of the entry with this exact path, or -1. */
int zim_catalog_find(const char *path);

#endif
