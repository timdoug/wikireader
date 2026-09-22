#ifndef LCD_H
#define LCD_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "mem.h"

/* Panel geometry from samo-lib/drivers/include/lcd.h and grifo.lds: rows are
 * padded to 256 bits (32 bytes) even though only 240 pixels are visible. */
#define LCD_WIDTH   240
#define LCD_HEIGHT  208
#define LCD_STRIDE  32

#define LCD_NREGS   64

struct lcd {
	uint32_t reg[LCD_NREGS];
	uint32_t fb_addr;        /* last value written to REG_LCDC_MADD */
	unsigned long writes;
	unsigned long stops;     /* transitions out of panel-driving mode */
	unsigned long starts;    /* and back into it */
	/* Host-side wiring, kept across lcd_reset: WREMU_LCD_TRACE=1 logs every
	 * framebuffer-address write with the MCLK time, which is one line per
	 * displayed frame while the firmware scrolls by repointing MADD. */
	const uint64_t *clk;
	bool trace;
};

void lcd_attach(struct mem *m, struct lcd *l);
void lcd_reset(struct lcd *l);
/*
 * True once the controller is actually refreshing the panel. LCD_initialise
 * (samo-lib/drivers/src/lcd.c) parks REG_LCDC_PS in PSAVE_POWER_SAVE, programs
 * the timing and the framebuffer address, and only then selects PSAVE_NORMAL.
 * Until that last write the glass is not being driven and shows nothing --
 * which is why a power cycle must not flash whatever the framebuffer still
 * holds from last time.
 */
bool lcd_driving(const struct lcd *l);
/* Composited panel pixel: 1 = black. Honours the PIP sub-window overlay. */
unsigned lcd_pixel(struct lcd *l, struct mem *m, int x, int y);
/*
 * Cheap fingerprint of everything the panel is showing, for deciding
 * whether a repaint is worth doing at all.
 */
uint64_t lcd_fingerprint(struct lcd *l, struct mem *m);
bool lcd_write_pgm(struct lcd *l, struct mem *m, const char *path);
void lcd_dump_ascii(struct lcd *l, struct mem *m, FILE *out);

#endif /* LCD_H */
