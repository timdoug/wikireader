/* Exercise the production source; redirect only its error helper so the
 * test can observe the C entry invariant before any prologue. */
#define __syscall_ret wr_checked_syscall_ret
#include "../musl/overlay/src/process/c33/vfork.S"
