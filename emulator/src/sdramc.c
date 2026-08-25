/*
 * SDRAM controller (REG_BASE+0x1600).
 *
 * The RAM itself is modelled in mem.c; this is only the control block, and
 * it exists because the boot path polls it. The EEPROM boot chain brings up
 * SDRAM before it can load anything large, and spins on
 *
 *     while ((REG_SDRAMC_INI & SDEN) == 0)
 *
 * which never completes if the register reads as zero.
 *
 * SDEN (D3) is read-only: "This bit indicates that the SDRAM has finished
 * initialization (Mode Register Set). ... SDEN is reset to 0 after power-on,
 * and is set to 1 upon completion of the initialization sequence."
 * (S1C33E07 Technical Manual, 0x301600). The sequence is PALL, REF then MRS
 * with the controller enabled, so the MRS command is what sets it.
 */

#include <string.h>

#include "sdramc.h"

#define OFF_INI  ((0x1600u - SDRAMC_BASE) / 4)
#define OFF_REF  ((0x1608u - SDRAMC_BASE) / 4)

#define SDON    (1u << 4)   /* controller enable        R/W */
#define SDEN    (1u << 3)   /* initialized flag         R   */
#define INIMRS  (1u << 2)   /* mode register set        R/W */

/*
 * Refresh register (0x301608). SELDO is read-only and reports whether the
 * SDRAM is actually in self-refresh:
 *
 *   D25 SELDO  SDRAM self-refresh status   1 Refresh mode  0 Done   R
 *   D23 SELEN  SDRAM self-refresh enable                            R/W
 *
 * grifo's suspend code, relocated into internal RAM, enables self-refresh
 * and then spins until SELDO reads back 1 before it powers things down.
 * With the bit unmodelled that loop never ends, and because Suspend()
 * disables interrupts first, nothing could wake it -- touch events queued
 * up and no interrupt was ever taken.
 */
#define SELDO   (1u << 25)  /* self-refresh status      R   */
#define SELEN   (1u << 23)  /* self-refresh enable      R/W */

static bool sdramc_mmio(void *ctx, uint32_t off, unsigned size, uint32_t *val,
			bool is_write)
{
	struct sdramc *s = ctx;
	uint32_t i = (off - SDRAMC_BASE) / 4;

	if (i >= SDRAMC_LEN / 4)
		return false;

	if (is_write) {
		if (i == OFF_REF) {
			s->reg[i] = *val & ~SELDO;   /* SELDO is read-only */
		} else if (i == OFF_INI) {
			/* SDEN is not writable; the MRS command raises it. */
			s->reg[i] = *val & ~SDEN;
			if ((*val & SDON) && (*val & INIMRS))
				s->initialised = true;
		} else {
			s->reg[i] = *val;
		}
		s->writes++;
		return true;
	}

	*val = s->reg[i];
	if (i == OFF_INI && s->initialised)
		*val |= SDEN;
	/* Self-refresh is entered and left as soon as it is asked for. */
	if (i == OFF_REF && (s->reg[i] & SELEN))
		*val |= SELDO;
	return true;
}

/* Reset state without re-registering the device. */
void sdramc_reset(struct sdramc *s)
{
	memset(s, 0, sizeof *s);
}

void sdramc_attach(struct mem *m, struct sdramc *s)
{
	sdramc_reset(s);
	mem_add_mmio(m, "sdramc", SDRAMC_BASE, SDRAMC_LEN, sdramc_mmio, s);
}
