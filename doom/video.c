/* Indexed colour to the WikiReader's 1-bit panel, GPL-3.0-or-later. */
#include <grifo.h>
#include <string.h>
#include "wr_doom.h"
#include "profile.h"

#ifdef WR_C33
#define FAST_CODE __attribute__((section(".fastcode"), noinline))
#define FAST_DATA __attribute__((section(".fastbss")))
#else
#define FAST_CODE
#define FAST_DATA
#endif

/* Each byte contains one four-pixel dither row, repeated twice. Keeping
   this small table and the conversion loop in internal RAM avoids SDRAM
   fetches between every source pixel and LCD write. */
static uint8_t black[4][256] FAST_DATA;
static uint8_t previous_palette[768];
static uint16_t source_y[WR_HEIGHT];
static uint8_t *framebuffer;
static int have_palette;
static const uint8_t bayer[16] = {
     0, 8, 2,10, 12, 4,14, 6, 3,11, 1, 9, 15, 7,13, 5
};

void wr_video_init(void)
{
    lcd_window_disable();
    lcd_set_default_framebuffer();
    lcd_clear(LCD_WHITE);
    framebuffer = lcd_get_framebuffer();
    for (unsigned y = 0; y < WR_HEIGHT; ++y) source_y[y] = y * 10 / 9;
    /* 4:3 image above a persistent, labelled touch control strip. */
    lcd_move_to(0, 180); lcd_line_to(239, 180);
    for (int x = 40; x < 240; x += 40) {
        lcd_move_to(x, 180); lcd_line_to(x, 207);
    }
    lcd_at_xy(0, 14); lcd_print("MENU MAP  <S   S>   GUN  RUN");
}

void wr_video_status(const char *text)
{
    if (!text) { wr_video_init(); return; }
    uint8_t *fb = lcd_get_framebuffer();
    memset(fb + 180 * WR_STRIDE, 0, 28 * WR_STRIDE);
    lcd_at_xy(0, 14); lcd_print(text);
}

static void FAST_CODE convert_frame(const uint8_t *source)
{
    for (unsigned y = 0; y < WR_HEIGHT; ++y) {
        const uint8_t *row = source + source_y[y] * 320;
        const uint8_t *lookup = black[y & 3];
        uint8_t *out = framebuffer + y * WR_STRIDE;
        /* 32 source pixels become 24 LCD pixels. The three sampling
           phases repeat exactly, eliminating a coordinate-table load and
           a loop branch for each pixel. Preserve the original sampling. */
#define PACK(a,b,c,d,e,f,g,h) \
        ((lookup[row[a]] & 0x80) | (lookup[row[b]] & 0x40) | \
         (lookup[row[c]] & 0x20) | (lookup[row[d]] & 0x10) | \
         (lookup[row[e]] & 0x08) | (lookup[row[f]] & 0x04) | \
         (lookup[row[g]] & 0x02) | (lookup[row[h]] & 0x01))
        for (unsigned group = 0; group < WR_WIDTH / 24; ++group) {
            out[0] = PACK(0,1,2,4,5,6,8,9);
            out[1] = PACK(10,12,13,14,16,17,18,20);
            out[2] = PACK(21,22,24,25,26,28,29,30);
            row += 32;
            out += 3;
        }
#undef PACK
    }
}

void wr_video_draw(const unsigned char *source, const unsigned char *palette)
{
    if (!have_palette || memcmp(previous_palette, palette, 768)) {
        memcpy(previous_palette, palette, 768);
        for (unsigned i = 0; i < 256; ++i) {
            unsigned gray = (77 * palette[i*3] + 150 * palette[i*3+1] + 29 * palette[i*3+2]) >> 8;
            /* Lift shadows on the unlit LCD while preserving black/white. */
            gray = gray * (510 - gray) / 255;
            for (unsigned y = 0; y < 4; ++y) {
                unsigned bits = 0;
                for (unsigned x = 0; x < 4; ++x)
                    bits = (bits << 1) | (gray < bayer[y * 4 + x] * 16 + 8);
                black[y][i] = (uint8_t)(bits | (bits << 4));
            }
        }
        have_palette = 1;
    }
    /* A0 is outside the C33 PC-relative call range. */
    void (*volatile convert)(const uint8_t *) = convert_frame;
    convert(source);
}
