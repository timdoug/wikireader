/* Code overlays in IVRAM; see zim_overlay.h. */
#include "zim_overlay.h"

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
	/* The overlays are stored halfword-aligned after .text and the buffer
	 * is word-aligned; the library memcpy shifts every word for that.
	 * Halfword copies, or word copies when the source happens to be
	 * aligned, are three times quicker for these 5 KB per phase. */
	if (((uintptr_t)load_start & 3) == 0) {
		const uint32_t *from = (const uint32_t *)load_start;
		uint32_t *to = (uint32_t *)ZIM_OVERLAY_BASE;
		size_t words = (size + 3) / 4;
		while (words--)
			*to++ = *from++;
	} else {
		const uint16_t *from = (const uint16_t *)load_start;
		uint16_t *to = (uint16_t *)ZIM_OVERLAY_BASE;
		size_t halves = (size + 1) / 2;
		while (halves--)
			*to++ = *from++;
	}
	resident = load_start;
}

void zim_overlay_invalidate(void)
{
	resident = NULL;
}
