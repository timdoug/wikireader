/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/*
 * Clocks of the Epson S1C33E07 clock-management unit ("epson,s1c33-cmu"):
 * MCLK, and the peripheral gates of GATEDCLK1 that drivers hold, the
 * timers' among them.
 */
#ifndef _DT_BINDINGS_CLOCK_S1C33_CMU_H
#define _DT_BINDINGS_CLOCK_S1C33_CMU_H

#define S1C33_CLK_MCLK		0
#define S1C33_CLK_SPI		1
#define S1C33_CLK_HSDMA		2
#define S1C33_CLK_EFSIO		3
#define S1C33_CLK_TM1		4
#define S1C33_CLK_ADC		5
#define S1C33_CLK_WDT		6
#define S1C33_CLK_TM0		7
#define S1C33_CLK_TM2		8
#define S1C33_CLK_TM3		9
#define S1C33_CLK_TM5		10

#endif
