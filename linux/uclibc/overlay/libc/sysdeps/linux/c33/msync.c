/* C33 supplies msync for resident no-MMU mappings.
 * Licensed under the LGPL v2.1, see the file COPYING.LIB in this tarball. */
#include <sys/syscall.h>
#include <sys/mman.h>
#include <cancel.h>

#define __NR___msync_nocancel __NR_msync
static _syscall3(int, __NC(msync), void *, addr, size_t, length, int, flags)

CANCELLABLE_SYSCALL(int, msync, (void *addr, size_t length, int flags),
                   (addr, length, flags))
