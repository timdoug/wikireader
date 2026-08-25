/*
 * LCD controller (REG_BASE+0x1a00), per samo-lib/include/regs.h and
 * samo-lib/drivers/src/lcd.c.
 *
 * The panel is 240x208, 1 bit per pixel. The framebuffer is not inside the
 * controller: grifo places it in ivram via the .lcd section of grifo.lds
 * (__BYTES_FrameBuffer = (256/8)*208, so rows are padded to 32 bytes) and
 * publishes its address by writing REG_LCDC_MADD. lcd_get_framebuffer then
 * hands that address back to the application:
 *
 *     REG_LCDC_MADD = LCD_VRAM;          (lcd.c:160)
 *     uint8_t *fb = (uint8_t *)REG_LCDC_MADD;   (lcd.c:189)
 *
 * So the one thing this device must do is remember what was written --
 * returning 0 for an unclaimed register handed the application a null
 * framebuffer pointer and sent it off into unmapped memory.
 */

#include <string.h>

#include "lcd.h"

#define LCDC_BASE   0x1a00u
#define LCDC_LEN    0x0100u

#define OFF_PS      (0x1a04u - LCDC_BASE)
#define OFF_DMD     (0x1a60u - LCDC_BASE)
#define OFF_MADD    (0x1a70u - LCDC_BASE)
#define OFF_MLADD   (0x1a74u - LCDC_BASE)
#define OFF_SADD    (0x1a80u - LCDC_BASE)
#define OFF_SSP     (0x1a88u - LCDC_BASE)
#define OFF_SEP     (0x1a8cu - LCDC_BASE)

#define R(l, off)   ((l)->reg[(off) / 4])

#define PIPEN       (1u << 31)
#define PIP_Y(v)    (((v) >> 16) & 0x3ff)
#define PIP_X(v)    ((v) & 0x3ff)

/* In 1-bpp mode one word of display data is 32 pixels, so the X coordinate
 * registers count 32-pixel units:
 *
 *   "The X coordinate should be specified with the number of data words
 *    converted from the number of pixels ... 1-bpp mode: 1-word = 32-pixel
 *    units"                     (S1C33E07 Technical Manual, VIII.1.6.5)
 */
#define PIXELS_PER_WORD 32

static bool lcd_mmio(void *ctx, uint32_t off, unsigned size, uint32_t *val,
		     bool is_write)
{
	struct lcd *l = ctx;
	uint32_t idx = (off - LCDC_BASE) / 4;

	if (idx >= LCD_NREGS)
		return false;

	if (is_write) {
		l->reg[idx] = *val;
		if (off - LCDC_BASE == OFF_MADD)
			l->fb_addr = *val;
		l->writes++;
		return true;
	}
	*val = l->reg[idx];
	return true;
}

/*
 * One composited panel pixel, 1 = black.
 *
 * The main window is read from MADD with a line stride of MLADD words. The
 * manual gives this device's exact case:
 *
 *   "if the LCD width and image width are 240 pixels in 1-bpp mode,
 *    MWLADR[9:0] = 240 x 1 / 32 = 7.5 [words]. In this case, MWLADR[9:0]
 *    must be set to 8. Furthermore, the image must be prepared in 256
 *    (8 x 32) pixels wide."             (Technical Manual, VIII.1.6.2)
 *
 * which is where the padded 32-byte stride comes from. MLADD is only
 * written by grifo's LCD_Window, so fall back to that stride while it reads
 * as its reset value of zero.
 *
 * If PIPEN is set, the sub-window replaces the main window inside its
 * rectangle. It has no line offset register of its own; its stride is its
 * own width, PIPXEND - PIPXST + 1 words, which is what grifo assumes --
 * LCD_Window computes WindowByteWidth = ((width + 31) >> 5) * 4 and its
 * PIPXST/PIPXEND differ by exactly width / 32.
 */
unsigned lcd_pixel(struct lcd *l, struct mem *m, int x, int y)
{
	uint32_t base = l->fb_addr;
	uint32_t mladd = R(l, OFF_MLADD) & 0x3ff;
	unsigned stride = mladd ? mladd * 4 : LCD_STRIDE;

	uint32_t ssp = R(l, OFF_SSP);
	if (ssp & PIPEN) {
		uint32_t sep = R(l, OFF_SEP);
		int x0 = PIP_X(ssp), x1 = PIP_X(sep);
		int y0 = PIP_Y(ssp), y1 = PIP_Y(sep);

		if (x >= x0 * PIXELS_PER_WORD &&
		    x < (x1 + 1) * PIXELS_PER_WORD &&
		    y >= y0 && y <= y1) {
			int sx = x - x0 * PIXELS_PER_WORD;
			int sy = y - y0;
			base = R(l, OFF_SADD);
			stride = (uint32_t)(x1 - x0 + 1) * 4;
			x = sx;
			y = sy;
		}
	}

	if (!base)
		return 0;

	unsigned v = mem_read(m, base + (uint32_t)y * stride + (x >> 3), 1);
	return (v >> (7 - (x & 7))) & 1;
}

/* Reset state without re-registering the device. */
/* REG_LCDC_PS[1:0]: 0 power save, 2 doze, 3 normal. */
#define PSAVE_MASK    0x3u
#define PSAVE_NORMAL  0x3u

bool lcd_driving(const struct lcd *l)
{
	return (R(l, OFF_PS) & PSAVE_MASK) == PSAVE_NORMAL;
}

void lcd_reset(struct lcd *l)
{
	memset(l, 0, sizeof *l);
	/*
	 * grifo does not publish the framebuffer through MADD: LCD.c's
	 * LCD_ResetFrameBuffer() loads the linker symbol directly
	 * ("xld.w %[v], __START_FrameBuffer"). The .lcd section is the only
	 * thing grifo.lds places in ivram, so that symbol resolves to the
	 * ivram base. The symbol itself is absent from grifo.elf because the
	 * build strips the section (--remove-section=.lcd).
	 */
	l->fb_addr = IVRAM_BASE;
	/*
	 * MADD must read back as the framebuffer address even before grifo
	 * writes it: LCD_clear() fetches it from the register rather than the
	 * symbol (xld.w %r4,0x301a70; ld.w %r5,[%r4]; ... ld.w [%r4],%r8).
	 * Returning 0 made it clear from address 0 and wipe the vector table
	 * at TTBR+0x34, after which the next syscall trapped to address 0.
	 */
	l->reg[(0x1a70u - LCDC_BASE) / 4] = IVRAM_BASE;
}

void lcd_attach(struct mem *m, struct lcd *l)
{
	lcd_reset(l);
	mem_add_mmio(m, "lcdc", LCDC_BASE, LCDC_LEN, lcd_mmio, l);
}

/*
 * Fingerprint the visible framebuffer, and the sub-window when it is
 * enabled, by hashing the bytes rather than the composited pixels. The
 * whole panel is 6656 bytes, so this costs far less than the repaint it
 * usually avoids: nothing draws to an idle screen, and presenting an
 * unchanged frame sixty times a second is work for the compositor as well
 * as for us.
 */
uint64_t lcd_fingerprint(struct lcd *l, struct mem *m)
{
	uint64_t h = 1469598103934665603ull;      /* FNV-1a */
	uint32_t mladd = R(l, OFF_MLADD) & 0x3ff;
	unsigned stride = mladd ? mladd * 4 : LCD_STRIDE;

	for (int y = 0; y < LCD_HEIGHT; y++)
		for (unsigned b = 0; b < stride; b++) {
			h ^= mem_read(m, l->fb_addr + (uint32_t)y * stride + b, 1);
			h *= 1099511628211ull;
		}

	uint32_t ssp = R(l, OFF_SSP);
	h ^= ssp; h *= 1099511628211ull;
	if (ssp & PIPEN) {
		uint32_t sep = R(l, OFF_SEP);
		int x0 = PIP_X(ssp), x1 = PIP_X(sep);
		int y0 = PIP_Y(ssp), y1 = PIP_Y(sep);
		unsigned sw = (unsigned)(x1 - x0 + 1) * 4;
		uint32_t base = R(l, OFF_SADD);
		h ^= sep; h *= 1099511628211ull;
		for (int y = y0; y <= y1; y++)
			for (unsigned b = 0; b < sw; b++) {
				h ^= mem_read(m, base + (uint32_t)(y - y0) * sw + b, 1);
				h *= 1099511628211ull;
			}
	}
	return h;
}

/*
 * Write the panel out as a binary PGM. Bit set means black: grifo's
 * image2header uses --inverted, and lcd.c drives a monochrome panel where a
 * 1 bit lights the pixel.
 */
bool lcd_write_pgm(struct lcd *l, struct mem *m, const char *path)
{
	if (!l->fb_addr)
		return false;

	FILE *fp = fopen(path, "wb");
	if (!fp)
		return false;

	fprintf(fp, "P5\n%d %d\n255\n", LCD_WIDTH, LCD_HEIGHT);
	for (int y = 0; y < LCD_HEIGHT; y++) {
		for (int x = 0; x < LCD_WIDTH; x++) {
			fputc(lcd_pixel(l, m, x, y) ? 0x00 : 0xff, fp);
		}
	}
	fclose(fp);
	return true;
}

/* Coarse ASCII view, 2x4 pixels per character, for quick sanity checks. */
void lcd_dump_ascii(struct lcd *l, struct mem *m, FILE *out)
{
	if (!l->fb_addr) {
		fprintf(out, "  (no framebuffer address published)\n");
		return;
	}
	static const char *ramp = " .:-=+*#%@";
	for (int y = 0; y < LCD_HEIGHT; y += 4) {
		fputs("  ", out);
		for (int x = 0; x < LCD_WIDTH; x += 2) {
			unsigned on = 0, tot = 0;
			for (int dy = 0; dy < 4 && y + dy < LCD_HEIGHT; dy++)
				for (int dx = 0; dx < 2 && x + dx < LCD_WIDTH; dx++) {
					on += lcd_pixel(l, m, x + dx, y + dy);
					tot++;
				}
			fputc(ramp[tot ? on * 9 / tot : 0], out);
		}
		fputc('\n', out);
	}
}
