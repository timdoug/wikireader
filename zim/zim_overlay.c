/* Code overlays in IVRAM; see zim_overlay.h. */
#include "zim_overlay.h"

#include <string.h>

static const void *resident;

void zim_overlay_ensure(const void *load_start, const void *load_stop)
{
	size_t size = (size_t)((const unsigned char *)load_stop -
			       (const unsigned char *)load_start);

	if (resident == load_start)
		return;
	/* application.lds asserts every overlay fits the buffer. */
	if (size > ZIM_OVERLAY_SIZE)
		size = ZIM_OVERLAY_SIZE;
	memcpy(ZIM_OVERLAY_BASE, load_start, size);
	resident = load_start;
}

void zim_overlay_invalidate(void)
{
	resident = NULL;
}
