/* Exercise the production source; redirect only its errno helper so the
 * test can observe the C entry invariant before any prologue. */
#define __errno_location wr_checked_errno_entry
#include "../uclibc/overlay/libc/sysdeps/linux/c33/vfork.S"
