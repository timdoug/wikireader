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
#define OFF_UPPER        (0x558u - AD_BLOCK)
#define OFF_LOWER        (0x55au - AD_BLOCK)
#define OFF_CH04_INTMASK (0x55cu - AD_BLOCK)
#define OFF_ADVMODE      (0x55eu - AD_BLOCK)

/* Reset values from the S1C33E07 Technical Manual register tables. */
#define EN_SMPL_STAT_RESET 0x0310u
#define CH04_INTMASK_RESET 0x001fu

#define ADSTART     (1u << 1)   /* EN_SMPL_STAT bit 1 starts a conversion */
#define ADENABLE    (1u << 2)
#define ADF         (1u << 3)
#define OWE         (1u << 0)
#define CONTINUOUS  (1u << 5)   /* TRIG_CHNL conversion mode */

#define CLKCTL_WRITABLE  0x000fu
#define TRIG_WRITABLE    0x3f38u
#define CTRL_WRITABLE    0xf376u

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

static void adc_convert(struct periph *p)
{
	uint16_t chnl = p->reg[OFF_TRIG_CHNL / 2];
	unsigned first = CS(chnl), last = CE(chnl);
	uint8_t swept = 0;

	if (last >= AD_CHANNELS)
		last = AD_CHANNELS - 1;
	for (unsigned ch = first; ch <= last; ch++) {
		swept |= 1u << ch;
		p->data[ch] = adc_count(ch);

		/* The standard register is overwritten once per channel. */
		if (p->add_adf)
			p->add_owe = 1;
		p->add = p->data[ch];
		p->add_adf = 1;
	}

	if (p->adf & swept) {
		p->owe |= p->adf & swept;
		p->overwrites++;
	}
	p->adf |= swept;
	p->conversions++;

	/* Normal mode stops after one selected-channel sweep. */
	if (!(chnl & CONTINUOUS))
		p->reg[OFF_EN_SMPL_STAT / 2] &= ~ADSTART;
}

static uint16_t adc_read_reg(struct periph *p, uint32_t reg)
{
	uint32_t idx = reg / 2;

	switch (reg) {
	case OFF_ADD:
		return p->add;
	case OFF_EN_SMPL_STAT:
		return (p->reg[idx] & CTRL_WRITABLE) |
		       (p->add_adf ? ADF : 0) | (p->add_owe ? OWE : 0);
	case OFF_END:
		return (uint16_t)p->adf | ((uint16_t)p->owe << 8);
	case OFF_UPPER:
	case OFF_LOWER:
		return p->reg[idx] & 0x03ff;
	case OFF_CH04_INTMASK:
		return p->reg[idx] & 0x001f;
	case OFF_ADVMODE:
		return p->reg[idx] & 0x0100;
	default:
		if (reg >= OFF_CH0_BUF &&
		    reg < OFF_CH0_BUF + AD_CHANNELS * 2)
			return p->data[(reg - OFF_CH0_BUF) / 2];
		return idx < 0x10 ? p->reg[idx] : 0;
	}
}

static void adc_write_reg(struct periph *p, uint32_t reg, uint16_t value,
			  uint16_t lanes)
{
	uint32_t idx = reg / 2;
	uint16_t mask;

	switch (reg) {
	case OFF_ADD:
		return;                         /* read-only */
	case OFF_TRIG_CHNL:
		mask = TRIG_WRITABLE & lanes;
		p->reg[idx] = (p->reg[idx] & ~mask) | (value & mask);
		return;
	case OFF_EN_SMPL_STAT:
		mask = CTRL_WRITABLE & lanes;
		p->reg[idx] = (p->reg[idx] & ~mask) | (value & mask);
		if ((lanes & OWE) && !(value & OWE))
			p->add_owe = 0;               /* write zero to clear */
		if ((p->reg[idx] & (ADSTART | ADENABLE)) ==
		    (ADSTART | ADENABLE) &&
		    !(p->reg[OFF_TRIG_CHNL / 2] & (3u << 3)))
			adc_convert(p);
		return;
	case OFF_END:
		/* OWEx is write-zero-to-clear; ADFx is read-only. */
		p->owe &= ~((~value & lanes) >> 8) & 0x1f;
		return;
	case OFF_UPPER:
	case OFF_LOWER:
		mask = 0x03ff & lanes;
		p->reg[idx] = (p->reg[idx] & ~mask) | (value & mask);
		return;
	case OFF_CH04_INTMASK:
		mask = 0x001f & lanes;
		p->reg[idx] = (p->reg[idx] & ~mask) | (value & mask);
		return;
	case OFF_ADVMODE:
		mask = 0x0100 & lanes;
		p->reg[idx] = (p->reg[idx] & ~mask) | (value & mask);
		return;
	default:
		return;                         /* buffers/gaps are read-only */
	}
}

static bool adc_mmio(void *ctx, uint32_t off, unsigned size, uint32_t *val,
		     bool is_write)
{
	struct periph *p = ctx;

	if (off == AD_CLKCTL || off == AD_CLKCTL + 1) {
		uint16_t lanes = size == 1 ? (off & 1 ? 0xff00 : 0x00ff) : 0xffff;
		uint16_t value = size == 1 && (off & 1) ? *val << 8 : *val;

		if (is_write) {
			uint16_t mask = CLKCTL_WRITABLE & lanes;
			p->clkctl = (p->clkctl & ~mask) | (value & mask);
			p->adc_writes++;
		} else {
			*val = size == 1 && (off & 1) ? p->clkctl >> 8 : p->clkctl;
		}
		return true;
	}

	if (off < AD_BLOCK || off >= AD_BLOCK + AD_BLOCK_LEN ||
	    (size != 1 && size != 2))
		return false;

	uint32_t reg = (off - AD_BLOCK) & ~1u;
	uint16_t lanes = size == 1 ? (off & 1 ? 0xff00 : 0x00ff) : 0xffff;
	uint16_t value = size == 1 && (off & 1) ? *val << 8 : *val;

	if (is_write) {
		p->adc_writes++;
		adc_write_reg(p, reg, value, lanes);
		return true;
	}

	uint16_t result = adc_read_reg(p, reg);
	*val = size == 1 && (off & 1) ? result >> 8 : result;

	if (reg >= OFF_CH0_BUF && reg < OFF_CH0_BUF + AD_CHANNELS * 2) {
		unsigned ch = (reg - OFF_CH0_BUF) / 2;
		/* "ADFx is reset to 0 when the converted data is read." */
		p->adf &= ~(1u << ch);
	} else if (reg == OFF_ADD) {
		p->add_adf = 0;
	}
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
