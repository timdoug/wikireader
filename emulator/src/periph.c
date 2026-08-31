/*
 * A/D converter (REG_BASE+0x520 clock control, 0x540..0x55f data block).
 *
 * The S1C33E07 has a "5-ch. 10-bit A/D converter", and grifo agrees:
 * analog.c defines ADC_FULL_SCALE as 1024. Counts are therefore 0..1023.
 * This device previously returned 0x800 on every channel, which is twice
 * full scale and made grifo's conversions produce nonsense -- the
 * thermistor channel came out as -154 C, and wiki.app puts that on screen
 * (wiki/keyboard.c:617 reads ANALOG_TEMPERATURE_CENTI_CELCIUS).
 *
 * There is no analogue world to model, so each channel returns a fixed
 * count. The counts are chosen by inverting grifo's own formulas, so the
 * numbers the firmware computes are physically sensible:
 *
 *   ch0, battery (analog.c:120)
 *     mV = adc0 * VADC_MULTIPLIER * AVDD_MILLIVOLTS / (1024 * 128)
 *     with VADC_MULTIPLIER = 128 * (150k + 1000k) / 1000k = 147 and
 *     AVDD_MILLIVOLTS = 3000 for this board (boards/samo_a1.h), so
 *     832 -> 2799 mV. samo_a1.h calls 3000 mV full and 2250 mV low, so
 *     this reads as a healthy pair of cells rather than a flat one.
 *
 *   ch1, thermistor (analog.c:125)
 *     centi-C = (adc1 * -1129 + 766386) / 100, so 502 -> 1996 = 19.96 C.
 *
 *   ch2, LCD contrast (analog.c:122)
 *     mV = adc2 * 45826 / 1000, a 0..46.9 V range across the converter.
 *     Mid-scale 512 -> 23.5 V, which is an ordinary STN bias.
 *
 * The channel status register is modelled properly rather than being wired
 * to "always done": "ADFx is reset to 0 when the converted data is read"
 * and OWEx (Dx+8) flags a conversion landing on unread data. grifo's
 * StartADC sets EN_SMPL_STAT bit 1 to start and then polls ADF2, so the
 * flags have to be raised by the start and cleared by the buffer reads.
 */

#include <string.h>

#include "periph.h"

#define AD_CLKCTL   0x0520u
#define AD_BLOCK    0x0540u
#define AD_BLOCK_LEN 0x0020u

#define OFF_ADD          (0x540u - AD_BLOCK)
#define OFF_TRIG_CHNL    (0x542u - AD_BLOCK)
#define OFF_EN_SMPL_STAT (0x544u - AD_BLOCK)
#define OFF_END          (0x546u - AD_BLOCK)
#define OFF_CH0_BUF      (0x548u - AD_BLOCK)
#define OFF_CH04_INTMASK (0x55cu - AD_BLOCK)

/* Reset values from the S1C33E07 Technical Manual register tables. */
#define EN_SMPL_STAT_RESET 0x0310u
#define CH04_INTMASK_RESET 0x001fu

#define ADSTART     (1u << 1)   /* EN_SMPL_STAT bit 1 starts a conversion */

/*
 * TRIG_CHNL selects the range of channels a single conversion sweeps:
 * "Analog inputs can be A/D-converted successively from the channel set
 * using CS[2:0] (D[10:8]) to the channel set using these bits [CE[2:0],
 * D[13:11]] in one operation."
 *
 * grifo writes 0x1000, which is CS = 0 and CE = 2 -- exactly the three
 * channels ScanADC goes on to read.
 */
#define CS(v)       (((v) >> 8) & 0x7)
#define CE(v)       (((v) >> 11) & 0x7)

uint16_t adc_count(unsigned channel)
{
	switch (channel) {
	case 0:  return AD_CH0_BATTERY;
	case 1:  return AD_CH1_THERMISTOR;
	case 2:  return AD_CH2_CONTRAST;
	default: return 512;        /* unused channels: mid-scale */
	}
}

static bool adc_mmio(void *ctx, uint32_t off, unsigned size, uint32_t *val,
		     bool is_write)
{
	struct periph *p = ctx;

	if (off == AD_CLKCTL) {
		if (is_write)
			p->clkctl = (uint16_t)*val, p->adc_writes++;
		else
			*val = p->clkctl;
		return true;
	}

	uint32_t reg = off - AD_BLOCK;
	uint32_t idx = reg / 2;

	if (is_write) {
		p->adc_writes++;
		if (reg == OFF_EN_SMPL_STAT && (*val & ADSTART)) {
			/*
			 * Starting a conversion overwrites every channel
			 * buffer. Any channel whose previous result was never
			 * read takes an overwrite error.
			 */
			uint16_t chnl = p->reg[OFF_TRIG_CHNL / 2];
			unsigned first = CS(chnl), last = CE(chnl);
			uint8_t swept = 0;

			if (last >= AD_CHANNELS)
				last = AD_CHANNELS - 1;
			for (unsigned ch = first; ch <= last; ch++)
				swept |= 1u << ch;

			/*
			 * A channel whose previous result was never read is
			 * overwritten by this sweep, which is what OWEx
			 * records. Channels outside the sweep keep whatever
			 * they had.
			 */
			if (p->adf & swept) {
				p->owe |= p->adf & swept;
				p->overwrites++;
			}
			p->adf |= swept;
			p->conversions++;
		}
		if (idx < 0x10)
			p->reg[idx] = (uint16_t)*val;
		return true;
	}

	if (reg == OFF_END) {
		*val = (uint32_t)p->adf | ((uint32_t)p->owe << 8);
		return true;
	}
	if (reg >= OFF_CH0_BUF && reg < OFF_CH0_BUF + AD_CHANNELS * 2) {
		unsigned ch = (reg - OFF_CH0_BUF) / 2;
		*val = adc_count(ch);
		/* "ADFx is reset to 0 when the converted data is read." */
		p->adf &= ~(1u << ch);
		p->owe &= ~(1u << ch);
		return true;
	}
	*val = idx < 0x10 ? p->reg[idx] : 0;
	return true;
}

void periph_reset(struct periph *p)
{
	memset(p, 0, sizeof *p);
	p->reg[OFF_EN_SMPL_STAT / 2] = EN_SMPL_STAT_RESET;
	p->reg[OFF_CH04_INTMASK / 2] = CH04_INTMASK_RESET;
}

void periph_attach(struct mem *m, struct periph *p)
{
	periph_reset(p);
	mem_add_mmio(m, "adc-clk", AD_CLKCTL, 2, adc_mmio, p);
	mem_add_mmio(m, "adc", AD_BLOCK, AD_BLOCK_LEN, adc_mmio, p);
}
