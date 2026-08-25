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

#define SDON    (1u << 4)   /* controller enable        R/W */
#define SDEN    (1u << 3)   /* initialized flag         R   */
#define INIMRS  (1u << 2)   /* mode register set        R/W */

static bool sdramc_mmio(void *ctx, uint32_t off, unsigned size, uint32_t *val,
			bool is_write)
{
	struct sdramc *s = ctx;
	uint32_t i = (off - SDRAMC_BASE) / 4;

	if (i >= SDRAMC_LEN / 4)
		return false;

	if (is_write) {
		if (i == OFF_INI) {
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
	return true;
}

void sdramc_attach(struct mem *m, struct sdramc *s)
{
	memset(s, 0, sizeof *s);
	mem_add_mmio(m, "sdramc", SDRAMC_BASE, SDRAMC_LEN, sdramc_mmio, s);
}
