/* Run work on the DSTRAM stack; see zim_overlay.h.  It is its own object at
 * the end of SOURCES so that adding it moved no existing SDRAM code. */
#include "zim_overlay.h"

/* noinline: the asm claims every call-clobbered register, which cannot be
 * satisfied where the compiler would inline it. */
__attribute__((noinline)) size_t
zim_call_on_dstram(size_t (*fn)(const void *), const void *args)
{
	size_t result;
	__asm__ volatile (
		"ld.w\t%%r1, %%sp\n\t"
		"xld.w\t%%r2, %3\n\t"
		"ld.w\t%%sp, %%r2\n\t"
		"ld.w\t%%r6, %2\n\t"
		"call\t%1\n\t"
		"ld.w\t%%sp, %%r1\n\t"
		"ld.w\t%0, %%r4"
		: "=r"(result)
		: "r"(fn), "r"(args), "i"(ZIM_DSTRAM_STACK_TOP)
		: "r1", "r2", "r4", "r5", "r6", "r7", "r8", "r9",
		  "r10", "r11", "r12", "r13", "r14", "memory");
	return result;
}
