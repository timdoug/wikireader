#include "pthread_impl.h"

/* Without an MMU, a trap pushes its 96-byte frame on the user stack, so
 * neither the munmap nor the exit may run on the stack being freed.  Both
 * run on this one; the thread list lock, which pthread_exit holds until
 * the kernel clears it at the thread's exit, keeps other threads off it. */
static unsigned long unmap_stack[64] __attribute__((__aligned__(16)));

void __unmapself(void *base, size_t size)
{
	register long r4 __asm__("r4") = SYS_munmap;
	register void *r6 __asm__("r6") = base;
	register size_t r7 __asm__("r7") = size;
	__asm__ __volatile__ (
		"ld.w %%sp,%3\n\t"
		"int 0\n\t"
		"ld.w %%r6,0\n\t"
		"xld.w %%r4,%4\n\t"
		"int 0\n"
		"1:\tjp 1b"
		: : "r"(r4), "r"(r6), "r"(r7),
		    "r"(unmap_stack + sizeof unmap_stack / sizeof *unmap_stack),
		    "i"(SYS_exit) : "memory");
	for (;;);
}
