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

void zim_overlay_invalidate(void)
{
	resident = NULL;
}
