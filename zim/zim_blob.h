/* Cluster decompression and blob retrieval for the small ZIM reader. */
#ifndef WIKIREADER_ZIM_BLOB_H
#define WIKIREADER_ZIM_BLOB_H

#include "zim_archive.h"

typedef void (*ZIM_BLOB_PROGRESS)(void *opaque, uint64_t completed,
				  uint64_t total);

int zim_archive_resolve_redirect(const ZIM_ARCHIVE *archive,
				 ZIM_DIRENT *dirent);

/* Drop the decoded-cluster cache, e.g. before switching archives. */
void zim_blob_cache_reset(void);

/* Read one blob into buffer. blob_size always receives the full decoded size.
 * If capacity is too small, the prefix which fits is returned and the result
 * is ZIM_ERR_TRUNCATED. A NULL buffer with zero capacity is a size query. */
int zim_archive_read_blob(const ZIM_ARCHIVE *archive,
			  const ZIM_DIRENT *dirent,
			  void *buffer, size_t capacity,
			  size_t *blob_size);

/* Borrow a blob straight from the decoded-cluster cache without copying it.
 * The pointer stays valid until the next blob call or cache reset.  Returns
 * ZIM_ERR_UNSUPPORTED when the blob cannot be served this way, in which case
 * the caller should read it into its own buffer instead. */
int zim_archive_view_blob_progress(const ZIM_ARCHIVE *archive,
				   const ZIM_DIRENT *dirent,
				   const unsigned char **data,
				   size_t *blob_size,
				   ZIM_BLOB_PROGRESS progress,
				   void *progress_opaque);

/* As above, reporting decompression/read work as it advances. */
int zim_archive_read_blob_progress(const ZIM_ARCHIVE *archive,
				   const ZIM_DIRENT *dirent,
				   void *buffer, size_t capacity,
				   size_t *blob_size,
				   ZIM_BLOB_PROGRESS progress,
				   void *progress_opaque);

#endif
