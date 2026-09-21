// SPDX-License-Identifier: GPL-2.0
#include <linux/clocksource.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/timekeeping.h>

#include <asm/wikireader.h>

#define C33_REG_BASE       0x00300000UL
#define C33_INT_P16T23     (C33_REG_BASE + 0x267)
#define C33_INT_E16T23     (C33_REG_BASE + 0x273)
#define C33_INT_F16T23     (C33_REG_BASE + 0x283)
#define C33_T16_CR2A       (C33_REG_BASE + 0x790)
#define C33_T16_CR2B       (C33_REG_BASE + 0x792)
#define C33_T16_CTL2       (C33_REG_BASE + 0x796)
#define C33_T16_PAUSE      (C33_REG_BASE + 0x7dc)
#define C33_T16_ADVMODE    (C33_REG_BASE + 0x7de)
#define C33_T16_CLKCTL2    (C33_REG_BASE + 0x7e4)
#define C33_CMU_GATE1      (C33_REG_BASE + 0x1b04)
#define C33_CMU_PROTECT    (C33_REG_BASE + 0x1b24)

#define C33_TIMER2_IRQ_BIT (1 << 2)
#define C33_BOOT_MCLK_HZ   60000000UL
#define C33_TIMER_DIV      64UL

void c33_timer_interrupt(void);

static inline unsigned char c33_read8(unsigned long address)
{
	return *(volatile unsigned char *)address;
}

static inline unsigned short c33_read16(unsigned long address)
{
	return *(volatile unsigned short *)address;
}

static inline unsigned long c33_read32(unsigned long address)
{
	return *(volatile unsigned long *)address;
}

static inline void c33_write8(unsigned char value, unsigned long address)
{
	*(volatile unsigned char *)address = value;
}

static inline void c33_write16(unsigned short value, unsigned long address)
{
	*(volatile unsigned short *)address = value;
}

static inline void c33_write32(unsigned long value, unsigned long address)
{
	*(volatile unsigned long *)address = value;
}

void c33_timer_interrupt(void)
{
	c33_write8(C33_TIMER2_IRQ_BIT, C33_INT_F16T23);
	legacy_timer_tick(1);
}

void __init time_init(void)
{
	unsigned long gate;
	unsigned int count = C33_BOOT_MCLK_HZ / C33_TIMER_DIV / HZ;

	c33_write32(0x96, C33_CMU_PROTECT);
	gate = c33_read32(C33_CMU_GATE1);
	c33_write32(gate | (1 << 15), C33_CMU_GATE1);
	c33_write32(0, C33_CMU_PROTECT);

	c33_write16(1, C33_T16_ADVMODE);
	c33_write16(2, C33_T16_CTL2);
	c33_write16(8 | 4, C33_T16_CLKCTL2);
	c33_write16(count - 1, C33_T16_CR2A);
	c33_write16(count - 1, C33_T16_CR2B);
	c33_write8((c33_read8(C33_INT_P16T23) & 0xf8) | 4,
		   C33_INT_P16T23);
	c33_write8(0x0c, C33_INT_F16T23);
	c33_write8(c33_read8(C33_INT_E16T23) | C33_TIMER2_IRQ_BIT,
		   C33_INT_E16T23);
	c33_write16(c33_read16(C33_T16_PAUSE) & ~(1 << 2),
		    C33_T16_PAUSE);
	c33_write16(2 | 1, C33_T16_CTL2);
	c33_lcd_checkpoint(3);
}

void read_persistent_clock64(struct timespec64 *ts)
{
	ts->tv_sec = 0;
	ts->tv_nsec = 0;
}
