// SPDX-License-Identifier: GPL-2.0
/* Clock-rate discovery for the WikiReader's S1C33E07. */
#include <linux/types.h>

#include <asm/wikireader.h>

#define C33_CMU_CLKCNTL 0x00301b08UL
#define C33_CMU_PLL     0x00301b0cUL

#define C33_OSC3_HZ     48000000UL
#define C33_OSC1_HZ     32768UL

unsigned long c33_mclk_hz(void)
{
	unsigned long ctl = *(volatile unsigned long *)C33_CMU_CLKCNTL;
	unsigned long hz;
	unsigned int source = (ctl >> 2) & 3;

	if (source == 1) {
		hz = C33_OSC1_HZ;
	} else if (source == 3) {
		unsigned long pll = *(volatile unsigned long *)C33_CMU_PLL;
		unsigned int input_div = ((ctl >> 20) & 15) + 1;

		/* Values 10..15 are reserved; the documented reset is /8. */
		if (input_div > 10)
			input_div = 8;
		switch (input_div) {
		case 1:  hz = C33_OSC3_HZ; break;
		case 2:  hz = C33_OSC3_HZ / 2; break;
		case 3:  hz = C33_OSC3_HZ / 3; break;
		case 4:  hz = C33_OSC3_HZ / 4; break;
		case 5:  hz = C33_OSC3_HZ / 5; break;
		case 6:  hz = C33_OSC3_HZ / 6; break;
		case 7:  hz = C33_OSC3_HZ / 7; break;
		case 8:  hz = C33_OSC3_HZ / 8; break;
		case 9:  hz = C33_OSC3_HZ / 9; break;
		default: hz = C33_OSC3_HZ / 10; break;
		}
		hz *= ((pll >> 4) & 15) + 1;
	} else {
		unsigned int shift = (ctl >> 8) & 7;

		/* OSC3DIV encodes /1,/2,/4,/8,/16,/32. */
		if (shift > 5)
			shift = 0;
		hz = C33_OSC3_HZ >> shift;
	}

	if (ctl & (1 << 12))
		hz >>= 1;
	return hz;
}
