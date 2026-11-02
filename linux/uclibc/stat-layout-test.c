/*
 * Compiled with -std=c99, where <sys/stat.h> cannot say struct timespec:
 * the structure must still be the one libc fills, which with 64-bit time
 * on the C33 is 116 bytes, the timestamps 12 apiece.
 */
#include <stddef.h>
#include <sys/stat.h>

_Static_assert(sizeof(struct stat) == 116, "struct stat differs from libc's");
_Static_assert(offsetof(struct stat, st_mtime) - offsetof(struct stat, st_atime) == 12,
	       "timestamps are not laid out as struct timespec");
