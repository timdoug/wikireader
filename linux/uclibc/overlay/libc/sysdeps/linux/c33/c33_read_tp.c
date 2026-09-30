/* Linux C33 thread pointer without a reserved general register.
 * Licensed under the LGPL v2.1, see COPYING.LIB. */
#include <features.h>

#ifdef IS_IN_rtld
__attribute__((visibility("hidden")))
#endif
void *__c33_read_tp(void)
{
	/* Compatibility entry for previously compiled binaries. New code uses
	 * get_thread_pointersi and libc's THREAD_SELF directly. */
	return __builtin_thread_pointer();
}
