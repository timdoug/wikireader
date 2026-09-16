/*
 * sdram - reprogram the SDRAM controller's timing after boot
 *
 * Copyright (c) 2026 Tim Douglas
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "standard.h"

#include <regs.h>
#include <samo.h>

#include "sdram.h"

/*
 * The boot loader in flash brings the SDRAM up with every timing field at
 * its maximum (samo-lib/include/boards/samo_a1.h: tRP 4, tRAS 8, tRC 15
 * clocks, refresh every 141) although its own comment works out 1, 3, and
 * 4 clocks for the part.  Each row change then costs about 19 clocks
 * instead of 7, and 13% of the bus goes to refresh.  The board's
 * EM48AM1684VTD-75 (Circuits/SAMO_PM_V3_SCH) needs tRP/tRCD 20 ns, tRAS
 * 45 ns, tRC/tRFC 65 ns, tXSR 75 ns, and a refresh every 7.8 us.
 *
 * The fields count SDCLK, and an SDCLK is two MCLK: the device times a read
 * that changes rows 8.5 MCLK above one that does not, against a programmed
 * tRP + tRCD of four of them (emulator/src/sdramc.c).  So a field clock is
 * 33.3 ns at the 60 MHz MCLK rather than 16.7, and the values below give 33,
 * 67 and 100 ns and a refresh every 7.5 us.  Sizing them against MCLK
 * instead halves every figure on paper, which reads as comfortably inside the
 * part while asking three times what it needs of the timings and refreshing
 * more slowly than it allows.
 *
 * Tightening them to what the part asks is worth, in the emulator, 4% of
 * coremark, 7% of dhrystone, 37% of the C33 assembly memcpy's throughput and
 * 6% of the time the terminal spends scrolling.  tRC 2 rather than 3 is
 * another half a percent and leaves 1.7 ns on a 65 ns requirement, which is
 * not enough to hold the same field's tRFC honestly.
 *
 * SDRAM_TIMING=STOCK leaves the loader's registers alone.  The values are
 * in sdram.h because SuspendCode, which puts the SDRAM into self-refresh
 * while the device sleeps between events, rewrites the refresh register on
 * every wake and must restore the same interval.
 */
/* Runs from A0 RAM so that no SDRAM access is in flight when the timing
 * changes, and none is started until the longest old interval has passed.
 * The same rules as SuspendCode apply: no stack, no calls, no globals. */
static void SDRAM_RetimeCode(uint32_t ctl, uint32_t ref)
	__attribute__((section(".suspend_text.retime"), noinline, used));

static void SDRAM_RetimeCode(uint32_t ctl, uint32_t ref)
{
	register int i;

	REG_SDRAMC_CTL = ctl;
	REG_SDRAMC_REF = ref;
	for (i = 0; i < 32; ++i) {
		asm volatile ("nop");
	}
}

void SDRAM_retime(void)
{
#if !defined(SDRAM_STOCK_TIMING)
	uint32_t ctl = (REG_SDRAMC_CTL & ADDRC_MASK)
		| ((SDRAM_CLKS_TRP - 1) << T24NS_SHIFT)
		| ((SDRAM_CLKS_TRAS - 1) << T60NS_SHIFT)
		| ((SDRAM_CLKS_TRC - 1) << T80NS_SHIFT);
	uint32_t ref = (REG_SDRAMC_REF & ~(0xfff << AURCO_SHIFT))
		| (SDRAM_REFRESH << AURCO_SHIFT);
	void (*code)(uint32_t, uint32_t);
	uint8_t *a0ram;
	const uint8_t *sdram;

	/* Suspend_initialise copied the whole .suspend section, this function
	 * included, to A0 RAM; call the copy. */
	asm volatile ("xld.w\t%[d], __START_SuspendCode\n\t"
		      "xld.w\t%[s], __START_suspend"
		      : [d] "=r" (a0ram), [s] "=r" (sdram));
	code = (void (*)(uint32_t, uint32_t))
		(a0ram + ((const uint8_t *)SDRAM_RetimeCode - sdram));
	code(ctl, ref);
#endif
}
