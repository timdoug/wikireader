#ifndef PERIPH_H
#define PERIPH_H

#include <stdbool.h>
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
	uint16_t data[AD_CHANNELS]; /* latched advanced-mode results */
	uint16_t add;               /* standard-mode result register */
	uint16_t clkctl;
	uint8_t  adf;               /* conversion-complete flags, one per ch */
	uint8_t  owe;               /* overwrite-error flags, one per ch */
	uint8_t  add_adf;           /* standard-mode completion flag */
	uint8_t  add_owe;           /* standard-mode overwrite flag */
	const uint64_t *clock;      /* the CPU's cycle count, if timed */
	const struct cmu *cmu;      /* MCLK, if modelled */
	bool     sweeping;          /* a conversion under way */
	unsigned channel, last;     /* the channel converting, the sweep's last */
	uint64_t channel_done;      /* when it finishes, in raw cycles */
	uint64_t channel_cycles;    /* one channel's conversion, in raw cycles */
	unsigned long adc_writes;
	unsigned long conversions;
	unsigned long overwrites;
};

void periph_reset(struct periph *p);
void periph_attach(struct mem *m, struct periph *p);
/* Time conversions by this clock; without one a sweep completes at once. */
void periph_set_clock(struct periph *p, const uint64_t *clock);
void periph_set_cmu(struct periph *p, const struct cmu *cmu);
/* Converter count for a channel, exposed for testing. */
uint16_t adc_count(unsigned channel);

#endif /* PERIPH_H */
