/*
 * Assorted simple peripherals.
 *
 * ADC (REG_BASE+0x520, 0x540..0x55f), per samo-lib/include/regs.h and
 * samo-lib/drivers/src/analog.c. grifo polls REG_AD_END waiting for a
 * conversion to finish; with no analogue world to model we report every
 * channel as permanently converted and hand back a mid-scale reading.
 *
 * The WikiReader uses these channels for battery voltage and the resistive
 * touch panel, so mid-scale reads as "battery fine, nothing pressed".
 */

#include <string.h>

#include "periph.h"

#define AD_CLKCTL   0x0520u
#define AD_BLOCK    0x0540u
#define AD_BLOCK_LEN 0x0020u

#define OFF_END     (0x546u - AD_BLOCK)
#define OFF_CH0     (0x548u - AD_BLOCK)

#define AD_ALL_DONE 0x001Fu   /* channels 0..4 conversion complete */
#define AD_MIDSCALE 0x0800u   /* 12-bit converter, mid range */

static bool adc_mmio(void *ctx, uint32_t off, unsigned size, uint32_t *val,
		     bool is_write)
{
	struct periph *p = ctx;

	if (is_write) {
		p->adc_writes++;
		return true;          /* configuration accepted, no effect */
	}

	if (off == AD_CLKCTL) {
		*val = 0;
		return true;
	}

	uint32_t reg = off - AD_BLOCK;
	if (reg == OFF_END) {
		*val = AD_ALL_DONE;
		return true;
	}
	if (reg >= OFF_CH0 && reg < OFF_CH0 + 10) {
		*val = AD_MIDSCALE;
		return true;
	}
	*val = 0;
	return true;
}

void periph_attach(struct mem *m, struct periph *p)
{
	memset(p, 0, sizeof *p);
	mem_add_mmio(m, "adc-clk", AD_CLKCTL, 2, adc_mmio, p);
	mem_add_mmio(m, "adc", AD_BLOCK, AD_BLOCK_LEN, adc_mmio, p);
}
