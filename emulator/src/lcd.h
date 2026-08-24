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
};

void lcd_attach(struct mem *m, struct lcd *l);
/* Composited panel pixel: 1 = black. Honours the PIP sub-window overlay. */
unsigned lcd_pixel(struct lcd *l, struct mem *m, int x, int y);
bool lcd_write_pgm(struct lcd *l, struct mem *m, const char *path);
void lcd_dump_ascii(struct lcd *l, struct mem *m, FILE *out);

#endif /* LCD_H */
