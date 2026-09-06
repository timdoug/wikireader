/*
 * simple 32bit counter incrementing at MCU MCLK
 *
 * Copyright (c) 2009 Openmoko Inc.
 *
 * Authors   Christopher Hall <hsw@openmoko.com>
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

#include "interrupt.h"
#include "CMU.h"
#include "power_log.h"
#include "timer.h"


void Timer_initialise(void)
{
	static bool initialised = false;
	if (!initialised) {
		initialised = true;
		CMU_initialise();

		Interrupt_type state = Interrupt_disable();

		CMU_enable1(TM5_CKE | TM0_CKE);

		// enable EXCL5 (input on P74)
		REG_P7_4_CFP = (REG_P7_4_CFP & ~0x03) | 0x02;

		// enable TM0  (output an P12)
		REG_P1_03_CFP = (REG_P1_03_CFP & ~0x03) | 0x01;

		// Advanced Mode
		REG_T16_ADVMODE = T16ADV;

		// 16 bit Timer 0
		// Stop timer
		REG_T16_CTL0 =
			//INITOLx |
			//SELFMx |
			//SELCRBx |
			OUTINVx |
			//CKSLx |
			PTMx |
			//PRESETx |
			//PRUNx |
			0;

		// Set prescale
		REG_T16_CLKCTL_0 =
			P16TONx |
			//P16TSx_MCLK_DIV_4096 |
			//P16TSx_MCLK_DIV_1024 |
			//P16TSx_MCLK_DIV_256 |
			//P16TSx_MCLK_DIV_64 |
			//P16TSx_MCLK_DIV_16 |
			//P16TSx_MCLK_DIV_4 |
			//P16TSx_MCLK_DIV_2 |
			P16TSx_MCLK_DIV_1 |
			0;

		// Set count
		REG_T16_CR0A = 0x7fff;
		REG_T16_CR0B = 0xffff;

		// Reset
		REG_T16_CTL0 |= PRESETx;


		// 16 bit Timer 5
		// Stop timer
		REG_T16_CTL5 =
			//INITOLx |
			//SELFMx |
			//SELCRBx |
			//OUTINVx |
			CKSLx |
			//PTMx |
			//PRESETx |
			//PRUNx |
			0;

		// Set prescale
		REG_T16_CLKCTL_5 =
			P16TONx |
			//P16TSx_MCLK_DIV_4096 |
			//P16TSx_MCLK_DIV_1024 |
			//P16TSx_MCLK_DIV_256 |
			//P16TSx_MCLK_DIV_64 |
			//P16TSx_MCLK_DIV_16 |
			//P16TSx_MCLK_DIV_4 |
			//P16TSx_MCLK_DIV_2 |
			P16TSx_MCLK_DIV_1 |
			0;

		// Set count
		REG_T16_CR5A = 0x7fff;
		REG_T16_CR5B = 0xffff;

		// Reset
		REG_T16_CTL5 |= PRESETx;

		// Set PAUSE On
		REG_T16_CNT_PAUSE =
			PAUSE5 |
			//PAUSE4 |
			//PAUSE3 |
			//PAUSE2 |
			//PAUSE1 |
			PAUSE0 |
			0;
		// Run
		REG_T16_CTL0 |= PRUNx;
		REG_T16_CTL5 |= PRUNx;

		// Set PAUSE Off
		REG_T16_CNT_PAUSE =
			//PAUSE5 |
			//PAUSE4 |
			//PAUSE3 |
			//PAUSE2 |
			//PAUSE1 |
			//PAUSE0 |
			0;

		Interrupt_enable(state);
	}
}


unsigned long Timer_get(void)
{
	register unsigned long high1, high2, low;

	do {
		Interrupt_type state = Interrupt_disable();
		high1 = REG_T16_TC5;
		low = REG_T16_TC0;
		high2 = REG_T16_TC5;
		Interrupt_enable(state);
	} while (high1 != high2);

	return (high2 << 16) | low;
}

void Timer_wait(unsigned long ticks)
{
	unsigned long gate = REG_CMU_GATEDCLK1;
	unsigned int priority = REG_INT_P16T23;
	unsigned int enable = REG_INT_E16T23;
	unsigned long count = (ticks + 1023) / 1024;
	bool log = PowerLog_enabled();
	unsigned long start;
	bool timeout;

	/* S1C33E07 manual III.1.11.1: enabled ITC causes release HALT
	 * even with PSR.IE clear. Leave MCLK, SDRAM, UARTs and GPIO alone;
	 * timers 0/5 must keep advancing the application's time base. */
	CMU_enable1(TM2_CKE);
	REG_T16_CTL2 = PRESETx;
	REG_T16_CLKCTL_2 = P16TONx | P16TSx_MCLK_DIV_1024;
	REG_T16_CR2A = count - 1;
	REG_T16_CR2B = count - 1;
	REG_INT_P16T23 = (priority & 0xf0) | 7;
	REG_INT_F16T23 = F16TC2 | F16TU2; /* write-one-to-clear, leave T3 alone */
	REG_INT_E16T23 = (enable & ~(E16TC2 | E16TU2)) | E16TU2;
	REG_T16_CTL2 = PRUNx;
	start = log ? Timer_get() : 0;
	asm volatile ("halt" : : : "memory");
	timeout = (REG_INT_F16T23 & F16TU2) != 0;
	REG_T16_CTL2 = 0;
	REG_INT_E16T23 = enable;
	REG_INT_F16T23 = F16TC2 | F16TU2;
	REG_INT_P16T23 = priority;
	REG_T16_CLKCTL_2 = 0;
	REG_CMU_PROTECT = CMU_PROTECT_OFF;
	REG_CMU_GATEDCLK1 = gate;
	REG_CMU_PROTECT = CMU_PROTECT_ON;
	if (log)
		PowerLog_idle(Timer_get() - start, timeout);
}
