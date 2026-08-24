/*
 * Interrupt controller.
 *
 * Only the register file plus a priority lookup: the priority registers are
 * read-modify-written by the drivers (grifo's CTP_initialise does
 * REG_INT_PSI01_PAD |= SERIAL_CH1_INT_PRI_7), so they have to read back what
 * was written or the surrounding fields are lost.
 *
 * Priorities feed the PSR's IL field. Per the C33 PE Core manual, "maskable
 * interrupt requests are accepted only when their priority levels are higher
 * than that set in the IL bit field", and IL is then set to the accepted
 * interrupt's level until reti restores the saved PSR.
 */

#include <string.h>

#include "itc.h"

/* Priority register offsets within the block, from samo-lib/include/regs.h. */
#define PSI01_PAD  (0x26au - ITC_BASE)   /* serial ch0 bits 6:4, ch1 bits 2:0 */

/* Vector numbers from samo-lib/grifo/src/vector.h. */
#define VEC_SERIAL0_ERR   56
#define VEC_SERIAL0_RX    57
#define VEC_SERIAL0_TX    58
#define VEC_SERIAL1_ERR   60
#define VEC_SERIAL1_RX    61
#define VEC_SERIAL1_TX    62

static bool itc_mmio(void *ctx, uint32_t off, unsigned size, uint32_t *val,
		     bool is_write)
{
	struct itc *t = ctx;
	uint32_t i = off - ITC_BASE;

	if (i + size > ITC_LEN)
		return false;

	if (is_write) {
		for (unsigned k = 0; k < size; k++)
			t->reg[i + k] = (uint8_t)(*val >> (8 * k));
		t->writes++;
		return true;
	}
	*val = 0;
	for (unsigned k = 0; k < size; k++)
		*val |= (uint32_t)t->reg[i + k] << (8 * k);
	return true;
}

unsigned itc_priority(const struct itc *t, unsigned vector)
{
	switch (vector) {
	case VEC_SERIAL0_ERR:
	case VEC_SERIAL0_RX:
	case VEC_SERIAL0_TX:
		return (t->reg[PSI01_PAD] >> 4) & 0x7;
	case VEC_SERIAL1_ERR:
	case VEC_SERIAL1_RX:
	case VEC_SERIAL1_TX:
		return t->reg[PSI01_PAD] & 0x7;
	default:
		/*
		 * Sources whose priority register is not decoded here. Report
		 * the maximum so they are never masked, which matches the
		 * previous behaviour of ignoring IL entirely.
		 */
		return 7;
	}
}

void itc_attach(struct mem *m, struct itc *t)
{
	memset(t, 0, sizeof *t);
	mem_add_mmio(m, "itc", ITC_BASE, ITC_LEN, itc_mmio, t);
}
