/* SD CSD capacity helpers shared by the target driver and host tests. */
#ifndef WIKIREADER_MMC_CSD_H
#define WIKIREADER_MMC_CSD_H

#include <inttypes.h>

static inline uint32_t mmc_csd_v2_sector_count(const uint8_t csd[16])
{
	uint32_t csize = ((uint32_t)(csd[7] & 0x3f) << 16) |
		((uint32_t)csd[8] << 8) | csd[9];

	/* CSD v2 expresses capacity in units of 1024 512-byte sectors. */
	return (csize + 1) << 10;
}

#endif
