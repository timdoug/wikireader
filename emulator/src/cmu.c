/*
 * Clock management unit (REG_BASE+0x1b00).
 *
 * Two things make this worth modelling rather than swallowing writes.
 *
 * First, the registers are read-modify-written: CMU_enable1() does
 * "REG_CMU_GATEDCLK1 |= mask" (samo-lib/grifo/src/CMU.c:287), so a register
 * that reads back as zero silently turns off every clock enabled earlier.
 *
 * Second, writes are gated. CMU_PROTECT takes 0x96 to unlock and 0x00 to
 * lock, and every driver here brackets its CMU accesses with that pair.
 * Honouring the gate turns a missing unlock into a visible rejected write
 * rather than a change that silently takes effect.
 *
 * The clock tree is not simulated -- there is one instruction stream and it
 * runs at whatever cycle costs c33.c charges -- but the configuration is
 * decoded, so the frequency the firmware asked for can be checked against
 * the timebase the emulator assumes. grifo programs OSCSEL_PLL with the PLL
 * at 48 MHz / 8 x 10 = 60 MHz, which is exactly the 60 MHz that
 * Tick_TicksPerMicroSecond and TIMER_CountsPerMicroSecond assume.
 */

#include <string.h>

#include "cmu.h"

#define OFF_GATEDCLK0  (0x1b00u - CMU_BASE)
#define OFF_GATEDCLK1  (0x1b04u - CMU_BASE)
#define OFF_CLKCNTL    (0x1b08u - CMU_BASE)
#define OFF_PLL        (0x1b0cu - CMU_BASE)
#define OFF_SSCG       (0x1b10u - CMU_BASE)
#define OFF_OPT        (0x1b14u - CMU_BASE)
#define OFF_PROTECT    (0x1b24u - CMU_BASE)

#define PROTECT_OFF    0x96

/* CLKCNTL fields (samo-lib/include/regs.h). */
#define OSC3DIV_SHIFT  8
#define MCLKDIV        (1u << 12)
#define SOSC3          (1u << 1)
#define SOSC1          (1u << 0)
#define OSCSEL(v)      (((v) >> 2) & 0x3)
#define OSCSEL_OSC3    0
#define OSCSEL_OSC1    1
#define OSCSEL_OSC3X   2
#define OSCSEL_PLL     3

/* PLL fields. */
#define PLLN(v)        ((((v) >> 4) & 0xf) + 1)    /* PLLN_X1 == 0 */
#define PLLPOWR        (1u << 0)
#define WAKEUPWT       (1u << 0)

static bool cmu_mmio(void *ctx, uint32_t off, unsigned size, uint32_t *val,
		     bool is_write)
{
	struct cmu *c = ctx;
	uint32_t reg = off - CMU_BASE;
	uint32_t idx = reg / 4;

	if (idx >= CMU_LEN / 4)
		return false;

	if (is_write) {
		/*
		 * "block further CMU access": once PROTECT holds anything but
		 * 0x96, writes to the other registers are discarded. PROTECT
		 * itself always accepts, or there would be no way back.
		 */
		if (reg != OFF_PROTECT &&
		    (c->reg[OFF_PROTECT / 4] & 0xff) != PROTECT_OFF) {
			c->blocked++;
			return true;
		}
		c->reg[idx] = *val;
		c->writes++;
		return true;
	}
	*val = c->reg[idx];
	return true;
}

uint32_t cmu_mclk_hz(const struct cmu *c)
{
	uint32_t clkcntl = c->reg[OFF_CLKCNTL / 4];
	uint32_t pll = c->reg[OFF_PLL / 4];
	uint32_t hz;

	switch (OSCSEL(clkcntl)) {
	case OSCSEL_OSC1:
		hz = clkcntl & SOSC1 ? OSC1_HZ : 0;
		break;
	case OSCSEL_OSC3:
	case OSCSEL_OSC3X: {
		static const uint8_t div[8] = { 1, 2, 4, 8, 16, 32, 1, 1 };
		unsigned setting = (clkcntl >> OSC3DIV_SHIFT) & 0x7;
		hz = clkcntl & SOSC3 ? OSC3_HZ / div[setting] : 0;
		break;
	}
	case OSCSEL_PLL:
	default:
		if (!(pll & PLLPOWR) || !(clkcntl & SOSC3)) {
			hz = 0;           /* PLL or its source is not powered */
			break;
		}
		/*
		 * The reference is OSC3 divided by PLLINDIV, and the output
		 * is that multiplied by PLLN. grifo's 48 MHz / 8 x 10 gives
		 * 60 MHz; PLLV sets the VCO divider, which the manual's own
		 * worked figures fold into the same result.
		 */
		unsigned setting = (clkcntl >> 20) & 0xf;
		unsigned indiv = setting <= 9 ? setting + 1 : 8;
		hz = OSC3_HZ / indiv * PLLN(pll);
		break;
	}

	return hz / (clkcntl & MCLKDIV ? 2 : 1);
}

bool cmu_slp_auto_wake(const struct cmu *c)
{
	/* Zero is the E07's automatic clock-switch wake mode. */
	return !(c->reg[OFF_OPT / 4] & WAKEUPWT);
}

void cmu_reset(struct cmu *c)
{
	memset(c, 0, sizeof *c);

	/* S1C33E07 Technical Manual, CMU register tables, init. column. */
	c->reg[OFF_GATEDCLK0 / 4] = 0x00000008; /* DSTRAM_CKE */
	c->reg[OFF_GATEDCLK1 / 4] = 0x3f0fffff; /* every implemented clock on */
	c->reg[OFF_CLKCNTL / 4] = 0x00770003;   /* both OSCs on, /8 defaults */
	c->reg[OFF_PLL / 4] = 0x00101804;       /* fixed analogue defaults */
	c->reg[OFF_SSCG / 4] = 0x0000f000;      /* interval timer default */
}

void cmu_attach(struct mem *m, struct cmu *c)
{
	cmu_reset(c);
	/*
	 * Reset state is locked, matching the hardware: the first thing every
	 * driver does is write 0x96.
	 */
	mem_add_mmio(m, "cmu", CMU_BASE, CMU_LEN, cmu_mmio, c);
}
