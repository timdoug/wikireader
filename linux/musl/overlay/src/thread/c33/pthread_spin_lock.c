#include "pthread_impl.h"
#include <errno.h>

/* One core: a contended owner cannot run until the waiter yields or is
 * preempted, so yield rather than spin out the rest of a tick. */
int pthread_spin_lock(pthread_spinlock_t *s)
{
	while (a_cas(s, 0, EBUSY))
		__syscall(SYS_sched_yield);
	return 0;
}
