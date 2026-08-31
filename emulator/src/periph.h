#ifndef PERIPH_H
#define PERIPH_H

#include <stdint.h>

#include "mem.h"

#define AD_CHANNELS 5

/*
 * Converter counts presented on each channel. 10 bits, so 0..1023.
 *
 * These are derived by inverting grifo's own conversions in
 * samo-lib/grifo/src/analog.c, so that a physically sensible reading comes
 * back out the other side. See the derivations in periph.c.
 */
#define AD_CH0_BATTERY      832   /* -> 2799 mV  */
#define AD_CH1_THERMISTOR   502   /* -> 19.96 C  */
#define AD_CH2_CONTRAST     512   /* -> 23462 mV */

struct periph {
	uint16_t reg[0x10];         /* AD block, 0x540..0x55f, by halfword */
	uint16_t clkctl;
	uint8_t  adf;               /* conversion-complete flags, one per ch */
	uint8_t  owe;               /* overwrite-error flags, one per ch */
	unsigned long adc_writes;
	unsigned long conversions;
	unsigned long overwrites;
};

void periph_reset(struct periph *p);
void periph_attach(struct mem *m, struct periph *p);
/* Converter count for a channel, exposed for testing. */
uint16_t adc_count(unsigned channel);

#endif /* PERIPH_H */
