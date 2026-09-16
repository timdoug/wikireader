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
 * clocks, refresh every 140) although its own comment works out 1, 3, and
 * 4 clocks for the part -- and that comment has the clock right, at the
 * source rate, where this file had it wrong for a day.  Left alone the
 * fields are two to three times what the part asks and the refresh three
 * times more often than it needs.  The board's
 * EM48AM1684VTD-75 (Circuits/SAMO_PM_V3_SCH) needs tRP/tRCD 20 ns, tRAS
 * 45 ns, tRC/tRFC 65 ns, tXSR 75 ns, and a refresh every 7.8 us.
 *
 * The fields count SDCLK, and an SDCLK is one MCLK -- 16.7 ns at 60 MHz.
 * III.1.9.4 of the technical manual has the SDRAM interface running on
 * OSC_W, the clock MCLK is divided from, and CMU.c leaves MCLKDIV at 0, so
 * the two are the same; II.4.4.7 says as much from the other side, that DBF
 * is for when they are not.  An earlier version of this file said two, from
 * a row change costing 8.5 MCLK against four programmed clocks -- which
 * says two only if the controller's own overhead is nothing, and it is
 * about six MCLK.  `ubench sdclk' settled it on the device by sweeping one
 * field at a time and taking the slope, where the overhead cancels: tRC
 * costs 1.17 MCLK a cycle, tRAS the same within the quantisation, and the
 * refresh interval scales the same way (tools/ubench-sdclk-device.txt).
 *
 * So, against the part's 20 / 45 / 65 ns and a 7.8 us refresh: tRP and tRCD
 * 2 cycles (33.3 ns), tRAS 3 (50 ns), and 5 rather than 4 for the third
 * field because it programs tXSR as well, and leaving self-refresh wants
 * 75 ns where tRC and tRFC want 65.  AURCO 0x1c0 refreshes every 7.47 us,
 * just inside the part and nearly twice as far apart as the loader's.
 *
 * The values this file shipped before -- 1, 2, 3 -- came from the same
 * factor of two and were 16.7 / 33.3 / 50 ns: under the part on all three,
 * and under tXSR by more than half.  They ran, as out-of-spec memory
 * timings do until they do not.  Honouring the part costs what the sweep
 * measures directly: a row change in the same bank goes from 15.19 to
 * 17.63 MCLK, the tRP field being the one that moves it, at 2.56 MCLK a
 * cycle because it is charged twice.  The refresh going the other way
 * gives about half a MCLK of that back.
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
