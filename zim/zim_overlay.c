/* Code overlays in IVRAM; see zim_overlay.h. */
#include "zim_overlay.h"
#include "zim_copy.h"

#include <stdint.h>
#include <string.h>

static const void *resident;

unsigned char zim_fast_scratch[ZIM_FAST_SCRATCH_SIZE]
	__attribute__((section(".fastbss"), aligned(4)));

void zim_overlay_ensure(const void *load_start, const void *load_stop)
{
	size_t size = (size_t)((const unsigned char *)load_stop -
			       (const unsigned char *)load_start);

	if (resident == load_start)
		return;
	/* application.lds asserts every overlay fits the buffer. */
	if (size > ZIM_OVERLAY_SIZE)
		size = ZIM_OVERLAY_SIZE;
	/* The linker word-aligns every overlay. The app's copy dispatcher
	 * uses the verified SDRAM-to-IVRAM DMA path for large overlays. */
	zim_copy(ZIM_OVERLAY_BASE, load_start, size);
	resident = load_start;
}

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

void zim_overlay_invalidate(void)
{
	resident = NULL;
}
