/*
 * A/D converter test.
 *
 * The point of this one is not the register plumbing but the numbers: the
 * counts the emulator presents are fed through grifo's own conversions
 * (samo-lib/grifo/src/analog.c) and the results checked for being physically
 * sensible. A 12-bit mid-scale of 0x800 passes every plumbing test and still
 * reports -154 C.
 */

#include <stdio.h>
#include <string.h>

#include "../src/periph.h"
#include "../src/mem.h"

/* analog.c and boards/samo_a1.h, transcribed. */
#define ADC_FULL_SCALE          1024
#define VADC_DIVISOR            128
#define ADC_SERIES_RESISTOR_K   150
#define ADC_SHUNT_RESISTOR_K    1000
#define AVDD_MILLIVOLTS         3000
#define VADC_MULTIPLIER (VADC_DIVISOR * (ADC_SERIES_RESISTOR_K + ADC_SHUNT_RESISTOR_K) / ADC_SHUNT_RESISTOR_K)

#define THERMISTOR_K0       766386
#define THERMISTOR_K1        -1129
#define THERMISTOR_DIVISOR   10000

#define CONTRAST_MULTIPLIER  45826
#define CONTRAST_DIVISOR      1000

/* boards/samo_a1.h battery thresholds. */
#define BATTERY_FULL  3000
#define BATTERY_LOW   2250
#define BATTERY_EMPTY 2100

static struct mem mem;
static struct periph adc;
static int fails;

static void check(const char *what, int got, int want)
{
	printf("%-62s %s\n", what, got == want ? "ok" : "FAIL");
	if (got != want) {
		printf("    got %d, wanted %d\n", got, want);
		fails++;
	}
}

static void range(const char *what, long v, long lo, long hi)
{
	int ok = v >= lo && v <= hi;
	printf("%-62s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok) {
		printf("    got %ld, wanted %ld..%ld\n", v, lo, hi);
		fails++;
	}
}

static uint32_t rd(uint32_t a)       { return mem_read(&mem, 0x300000u + a, 2); }
static void     wr(uint32_t a, uint32_t v) { mem_write(&mem, 0x300000u + a, 2, v); }
static uint32_t rd8(uint32_t a)      { return mem_read(&mem, 0x300000u + a, 1); }
static void     wr8(uint32_t a, uint32_t v) { mem_write(&mem, 0x300000u + a, 1, v); }

/* StartADC(), transcribed. */
static void start_adc(void)
{
	wr(0x520, 0x000f);
	wr(0x55e, 0x0100);
	wr(0x542, 0x1000);
	wr(0x544, 0x0304);
	wr(0x544, rd(0x544) | 0x02);
	int spins = 0;
	while (0 == (rd(0x546) & 0x04))       /* poll ADF2 */
		if (++spins > 1000) { printf("StartADC never completed\n"); fails++; break; }
}

int main(void)
{
	mem_init(&mem);
	periph_attach(&mem, &adc);
	check("EN_SMPL_STAT has its documented reset value",
	      rd(0x544), 0x0310);
	check("per-channel interrupt masks reset enabled",
	      rd(0x55c), 0x001f);

	wr(0x520, 0x000f);
	wr(0x544, 0);
	wr(0x55c, 0);
	periph_reset(&adc);
	check("power-on resets the A/D clock control", rd(0x520), 0);
	check("power-on restores EN_SMPL_STAT", rd(0x544), 0x0310);
	check("power-on restores per-channel masks", rd(0x55c), 0x001f);
	check("result buffers reset empty", rd(0x548), 0);
	wr(0x548, 0x03ff);
	check("writes to result buffers are ignored", rd(0x548), 0);
	wr(0x520, 0xffff);
	check("reserved clock-control bits read zero", rd(0x520), 0x000f);

	/* ADST is not accepted while the converter itself is disabled. */
	wr(0x544, 0x0302);
	check("a trigger with ADE clear performs no conversion", rd(0x546), 0);
	periph_reset(&adc);

	/* Every channel must fit the converter's 10 bits. */
	for (unsigned ch = 0; ch < AD_CHANNELS; ch++) {
		char buf[80];
		snprintf(buf, sizeof buf, "channel %u count is within 10 bits", ch);
		check(buf, adc_count(ch) < ADC_FULL_SCALE, 1);
	}

	start_adc();
	check("normal-mode conversion clears ADST", rd(0x544) & 0x02, 0);
	check("standard result reports completion", rd(0x544) & 0x08, 0x08);
	check("standard result holds the final selected channel",
	      rd(0x540), adc_count(2));
	check("reading standard result clears its ADF", rd(0x544) & 0x08, 0);
	wr(0x540, 0x03ff);
	check("writes to standard result are ignored", rd(0x540), adc_count(2));
	long a0 = rd(0x548), a1 = rd(0x54a), a2 = rd(0x54c);

	check("ADF2 is set by the conversion and cleared by reading CH2",
	      (rd(0x546) & 0x04) == 0, 1);
	check("no overwrite errors from a read-every-channel sweep",
	      adc.overwrites, 0);

	/* grifo's conversions, over the counts this device presents. */
	long mv   = (a0 * (VADC_MULTIPLIER * AVDD_MILLIVOLTS)) / (ADC_FULL_SCALE * VADC_DIVISOR);
	long cC   = (a1 * THERMISTOR_K1 + THERMISTOR_K0) / (THERMISTOR_DIVISOR / 100);
	long lcd  = (a2 * CONTRAST_MULTIPLIER) / CONTRAST_DIVISOR;

	printf("\n  battery %ld mV, temperature %ld.%02ld C, contrast %ld mV\n\n",
	       mv, cC / 100, cC % 100, lcd);

	range("battery reads between LOW and FULL", mv, BATTERY_LOW, BATTERY_FULL);
	range("temperature is an ordinary room temperature", cC, 1500, 2500);
	range("LCD bias is a plausible STN drive voltage", lcd, 10000, 30000);

	/*
	 * The regression this guards: 0x800 is what the device used to
	 * return on every channel.
	 */
	long bad = (0x800L * THERMISTOR_K1 + THERMISTOR_K0) / (THERMISTOR_DIVISOR / 100);
	check("a 12-bit mid-scale would have reported -154 C", bad / 100, -154);

	/* Channels outside CS..CE are not converted, so they cannot overwrite. */
	wr(0x542, 0x1000);                    /* CS = 0, CE = 2 */
	wr(0x544, rd(0x544) | 0x02);
	check("sweep sets ADF for channels 0..2 only", rd(0x546) & 0x1f, 0x07);

	wr(0x542, (4 << 11) | (3 << 8));      /* CS = 3, CE = 4 */
	wr(0x544, rd(0x544) | 0x02);
	check("changing CS/CE moves the sweep", rd(0x546) & 0x1f, 0x1f);

	unsigned long before = adc.overwrites;
	wr(0x544, rd(0x544) | 0x02);          /* sweep 3..4 again, unread */
	check("re-sweeping unread channels raises an overwrite error",
	      adc.overwrites > before, 1);
	check("OWE appears in the high half of the status register",
	      (rd(0x546) >> 8) & 0x1f, 0x18);
	rd(0x54e);
	check("reading a result leaves its OWE latched",
	      (rd(0x546) >> 8) & 0x1f, 0x18);
	wr(0x546, 0xffff);
	check("writing one does not clear OWE", (rd(0x546) >> 8) & 0x1f, 0x18);
	wr8(0x547, 0x00);
	check("writing zero clears OWE through a byte access",
	      (rd(0x546) >> 8) & 0x1f, 0);
	wr8(0x55f, 0x01);
	check("high-byte writes select advanced mode", rd8(0x55f), 0x01);

	printf("\n%s\n", fails ? "FAILURES" : "all ADC tests passed");
	return fails != 0;
}
