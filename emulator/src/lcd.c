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

#define OFF_MADD    (0x1a70u - LCDC_BASE)

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

void lcd_attach(struct mem *m, struct lcd *l)
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
	mem_add_mmio(m, "lcdc", LCDC_BASE, LCDC_LEN, lcd_mmio, l);
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
			uint32_t byte = l->fb_addr + y * LCD_STRIDE + (x >> 3);
			unsigned v = mem_read(m, byte, 1);
			unsigned bit = (v >> (7 - (x & 7))) & 1;
			fputc(bit ? 0x00 : 0xff, fp);
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
					uint32_t b = l->fb_addr +
						     (y + dy) * LCD_STRIDE +
						     ((x + dx) >> 3);
					unsigned v = mem_read(m, b, 1);
					on += (v >> (7 - ((x + dx) & 7))) & 1;
					tot++;
				}
			fputc(ramp[tot ? on * 9 / tot : 0], out);
		}
		fputc('\n', out);
	}
}
