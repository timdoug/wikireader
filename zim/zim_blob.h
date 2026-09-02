/* Cluster decompression and blob retrieval for the small ZIM reader. */
#ifndef WIKIREADER_ZIM_BLOB_H
#define WIKIREADER_ZIM_BLOB_H

#include "zim_archive.h"

int zim_archive_resolve_redirect(const ZIM_ARCHIVE *archive,
				 ZIM_DIRENT *dirent);

/* Read one blob into buffer. blob_size always receives the full decoded size.
 * If capacity is too small, the prefix which fits is returned and the result
 * is ZIM_ERR_TRUNCATED. A NULL buffer with zero capacity is a size query. */
int zim_archive_read_blob(const ZIM_ARCHIVE *archive,
			  const ZIM_DIRENT *dirent,
			  void *buffer, size_t capacity,
			  size_t *blob_size);

#endif
