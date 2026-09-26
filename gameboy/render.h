/*
 * The scanline renderer, included by gb.c after peanut_gb.h.
 *
 * A Game Boy tile row is two bytes, one per bit of the colour index, with
 * the leftmost pixel in bit 7 -- the panel's own layout.  A line is held
 * as five big-endian 32-bit words per bit plane, the leftmost pixel in bit
 * 31, so a fine scroll is one funnel shift a word, and palettes, sprite
 * transparency and priority and the dither are all masks: nothing loops
 * over pixels.  Peanut's own renderer spends about a hundred cycles a
 * pixel on the C33.
 *
 * It reproduces Peanut's __gb_draw_line exactly, including where Peanut
 * departs from the hardware: sprites behind the background compare shades
 * with BGP colour 0's shade rather than colour indexes, and the ten
 * sprites of a line are the ten with the lowest X.  A GBW_CHECK_RENDER
 * build runs both and compares every line.
 * SPDX-License-Identifier: MIT
 */

#ifdef __c33__
/* A0 RAM: no instruction-fetch stalls, and no SDRAM row for the data. */
#define RENDER_CODE __attribute__((section(".fastcode"), noinline))
#define RENDER_DATA __attribute__((section(".fastbss"), aligned(4)))
#else
#define RENDER_CODE
#define RENDER_DATA __attribute__((aligned(4)))
#endif

/* Shade planes, bit 0 and bit 1 of each pixel's shade: words 1 to 5 are
   the line; words 0 and 6 catch the parts of sprites hanging off either
   edge.  Byte k of the line, pixels 8k to 8k + 7, is at byte
   SHADE_BYTE(k) of a little-endian host's memory. */
enum { SHADE_WORDS = 7 };
#define SHADE_BYTE(k) (4 + ((k) ^ 3))
static uint32_t shade0[SHADE_WORDS] RENDER_DATA;
static uint32_t shade1[SHADE_WORDS] RENDER_DATA;
/* Tile-row streams, bit planes 0 and 1, big-endian: the background from
   its first visible column, the window with a blank word ahead of it. */
static uint32_t stream0[7] RENDER_DATA;
static uint32_t stream1[7] RENDER_DATA;
static uint32_t window0[7] RENDER_DATA;
static uint32_t window1[7] RENDER_DATA;
/* The line's colour-index planes and which pixels background or window
   cover, between render_line's passes. */
static uint32_t line_lo[5] RENDER_DATA;
static uint32_t line_hi[5] RENDER_DATA;
static uint32_t line_cover[5] RENDER_DATA;
/* A map row twice over, so 21 columns from any start read straight on. */
static uint32_t map_row[16] RENDER_DATA;
static uint8_t reversed[256] RENDER_DATA;

static inline __attribute__((always_inline)) uint32_t broadcast(unsigned bit)
{
	return -(uint32_t)(bit & 1);
}

/* A palette register as eight masks: shade bit b of colour c is
   mask[2 * c + b]. */
struct palette {
	uint32_t c0_0, c0_1, d10_0, d10_1, c2_0, c2_1, d32_0, d32_1;
};
/* Built when a palette register changes, not each line and sprite: the
   value each set was built for, or 0x100 for none yet. */
static struct palette bg_masks RENDER_DATA, sprite_masks[2] RENDER_DATA;
static unsigned bg_masks_for RENDER_DATA, sprite_masks_for[2] RENDER_DATA;

static inline __attribute__((always_inline)) void
palette_masks(unsigned pal, struct palette *p)
{
	p->c0_0 = broadcast(pal);
	p->c0_1 = broadcast(pal >> 1);
	p->d10_0 = broadcast(pal >> 2) ^ p->c0_0;
	p->d10_1 = broadcast(pal >> 3) ^ p->c0_1;
	p->c2_0 = broadcast(pal >> 4);
	p->c2_1 = broadcast(pal >> 5);
	p->d32_0 = broadcast(pal >> 6) ^ p->c2_0;
	p->d32_1 = broadcast(pal >> 7) ^ p->c2_1;
}

/* Colour-index planes to shade planes: each shade bit is a mux on the
   colour's two bits, x = b ^ ((a ^ b) & m) twice. */
#define SHADE(p, lo, hi, s0, s1) do {					\
	uint32_t low_ = (p).c0_0 ^ ((p).d10_0 & (lo));			\
	uint32_t high_ = (p).c2_0 ^ ((p).d32_0 & (lo));			\
	(s0) = low_ ^ ((high_ ^ low_) & (hi));				\
	low_ = (p).c0_1 ^ ((p).d10_1 & (lo));				\
	high_ = (p).c2_1 ^ ((p).d32_1 & (lo));				\
	(s1) = low_ ^ ((high_ ^ low_) & (hi));				\
} while (0)

/* The 32 pixels starting `shift` bits (0 to 31) into s[i]: a funnel
   shift, with the second half shifted in two steps so a zero shift does
   not shift by 32. */
static inline __attribute__((always_inline)) uint32_t
funnel(const uint32_t *s, unsigned i, unsigned shift)
{
	return s[i] << shift | (s[i + 1] >> 1) >> (31 - shift);
}

#ifdef GBW_CHECK_RENDER
static uint8_t saved_window_line;

/* Peanut's pixels against the shade planes render_line left. */
static void check_line(struct gb_s *g, const uint8_t *pixels,
		       const uint_fast8_t line)
{
	(void)g;
	for (unsigned x = 0; x < GBW_WIDTH; ++x) {
		unsigned byte = SHADE_BYTE(x / 8), bit = 7 - x % 8;
		unsigned shade = ((((uint8_t *)shade0)[byte] >> bit) & 1)
			| ((((uint8_t *)shade1)[byte] >> bit) & 1) << 1;

		if (shade != (pixels[x] & 3))
			gbw_error("render differs from Peanut's at line",
				  (unsigned)(line << 8 | x));
	}
}
#endif

static void build_reversed(void)
{
	for (unsigned i = 0; i < 256; ++i) {
		unsigned r = 0;

		for (int b = 0; b < 8; ++b)
			r |= ((i >> b) & 1) << (7 - b);
		reversed[i] = (uint8_t)r;
	}
}

/* Twenty-one tile columns of a map row, from column `first`, into a
   big-endian stream of six words a plane.  VRAM is in SDRAM, where a load
   that misses the open row costs 18-20 cycles, so the map row comes in as
   eight words and each tile row as one halfword. */
/* gb.c's vram array is word aligned, for the halfword tile loads. */

static inline __attribute__((always_inline)) void
fetch_stream(const uint8_t *map, unsigned first, unsigned tile_row,
	     unsigned lcdc, uint32_t *out0, uint32_t *out1)
{
	/* Unsigned tile numbers from 0x8000, or signed ones about 0x9000:
	   the same as unsigned from 0x8800 with the top bit flipped. */
	unsigned flip = lcdc & LCDC_TILE_SELECT ? 0 : 0x80;
	const uint8_t *tiles = gb.vram + tile_row
		+ (flip ? VRAM_TILES_2 : VRAM_TILES_1);
	const uint8_t *row = (const uint8_t *)map_row + first;
	uint32_t a0 = 0, a1 = 0;

	/* The planes as one halfword load: VRAM is word aligned and tile rows
	   are at even offsets, which the compiler cannot see and otherwise
	   splits into two byte loads from SDRAM. */
#define TILE() do {							\
		const uint16_t *at_ = __builtin_assume_aligned(		\
			tiles + ((*row++ ^ flip) << 4), 2);		\
		unsigned planes_ = *at_;				\
									\
		a0 = a0 << 8 | (planes_ & 0xff); /* little-endian: plane */ \
		a1 = a1 << 8 | planes_ >> 8;	 /* 0 at the lower address */ \
	} while (0)

	for (int i = 0; i < 8; ++i)
		map_row[i] = map_row[i + 8] = ((const uint32_t *)map)[i];
	for (unsigned w = 0; w < 5; ++w) {
		TILE();
		TILE();
		TILE();
		TILE();
		out0[w] = a0;
		out1[w] = a1;
	}
	TILE();
	out0[5] = a0 << 24;
	out1[5] = a1 << 24;
#undef TILE
}

/* Which sprites each line shows, in OAM order, rebuilt when OAM or the
   sprite size changes: scanning all forty for every line cost two SDRAM
   loads a sprite, 144 times a frame. */
static uint8_t line_count[LCD_HEIGHT] __attribute__((aligned(4)));
static uint8_t line_sprites[LCD_HEIGHT][NUM_SPRITES];
static int sprites_dirty = 1;

#ifdef __c33__
/* Once a frame or so: in the default framebuffer's code, beside hot.s. */
#define BUCKET_CODE __attribute__((section(".fbcode"), noinline))
#else
#define BUCKET_CODE __attribute__((noinline))
#endif

static BUCKET_CODE void bucket_sprites(unsigned lcdc)
{
	unsigned span = lcdc & LCDC_OBJ_SIZE ? 16 : 8;
	uint32_t key[NUM_SPRITES];

	/* In (X, number) order, so that each line's list is too: the ten
	   sprites Peanut shows on a line are its first ten, painted from the
	   last.  One sort of forty keys, X above the number, when OAM
	   changes, rather than one on every line with sprites. */
	for (unsigned s = 0; s < NUM_SPRITES; ++s) {
		uint32_t k = (uint32_t)gb.oam[4 * s + 1] << 8 | s;
		unsigned place = s;

		for (; place && key[place - 1] > k; --place)
			key[place] = key[place - 1];
		key[place] = k;
	}
	for (unsigned ly = 0; ly < LCD_HEIGHT; ly += 4)
		*(uint32_t *)&line_count[ly] = 0;
	for (unsigned i = 0; i < NUM_SPRITES; ++i) {
		unsigned s = key[i] & 0xff;
		/* On line ly when oy - 16 <= ly < oy - 16 + span. */
		int top = gb.oam[4 * s] - 16;

		for (int ly = top < 0 ? 0 : top; ly < top + (int)span && ly < LCD_HEIGHT; ++ly)
			line_sprites[ly][line_count[ly]++] = (uint8_t)s;
	}
	sprites_dirty = 0;
}

/* Called from A0 RAM, out of a direct call's reach of SDRAM. */
static void (*volatile rebucket)(unsigned lcdc) = bucket_sprites;

static RENDER_CODE void render_sprites(unsigned ly, unsigned lcdc)
{
	uint8_t order[MAX_SPRITES_LINE];
	unsigned count;
	unsigned tall = lcdc & LCDC_OBJ_SIZE;
	unsigned bg_zero = gb.hram_io[IO_BGP] & 3;
	uint32_t zero0 = broadcast(bg_zero), zero1 = broadcast(bg_zero >> 1);
	uint8_t *s0p = (uint8_t *)shade0, *s1p = (uint8_t *)shade1;

	/* The line's list is in (x, number) order (bucket_sprites): the
	   first ten are shown. */
	count = line_count[ly] < MAX_SPRITES_LINE ? line_count[ly] : MAX_SPRITES_LINE;
	for (unsigned i = 0; i < count; ++i)
		order[i] = line_sprites[ly][i];

	/* Lowest priority first, each painted over the last. */
	while (count--) {
		const uint8_t *o = &gb.oam[4 * order[count]];
		unsigned ox = o[1], flags = o[3];
		unsigned tile = o[2] & (tall ? 0xfe : 0xff);
		unsigned py = (ly - o[0] + 16) & 0xff;
		unsigned lo, hi, pal, shift;
		const struct palette *masks;
		int k;

		if (ox == 0 || ox >= 168)
			continue;
		if (flags & OBJ_FLIP_Y)
			py = ((tall ? 15 : 7) - py) & 0xff;
		lo = gb.vram[VRAM_TILES_1 + tile * 16 + 2 * py];
		hi = gb.vram[VRAM_TILES_1 + tile * 16 + 2 * py + 1];
		if (flags & OBJ_FLIP_X) {
			lo = reversed[lo];
			hi = reversed[hi];
		}
		{
			unsigned which = flags & OBJ_PALETTE ? 1 : 0;

			pal = gb.hram_io[which ? IO_OBP1 : IO_OBP0];
			if (sprite_masks_for[which] != pal) {
				palette_masks(pal, &sprite_masks[which]);
				sprite_masks_for[which] = pal;
			}
			masks = &sprite_masks[which];
		}
		/* The sprite's left edge is screen x ox - 8: line byte k,
		   from -1 (off the left) to 20 (off the right). */
		k = (int)(ox >> 3) - 1;
		shift = ox & 7;
		for (unsigned half = 0; half < 2; ++half, ++k) {
			unsigned at = (unsigned)SHADE_BYTE(k);
			uint32_t l = (half ? lo << (8 - shift) : lo >> shift) & 0xff;
			uint32_t h = (half ? hi << (8 - shift) : hi >> shift) & 0xff;
			uint32_t visible = l | h, s0, s1;

			if (!visible)
				continue;
			if (flags & OBJ_PRIORITY)
				visible &= ~((s0p[at] ^ zero0) | (s1p[at] ^ zero1));
			SHADE(*masks, l, h, s0, s1);
			s0p[at] = (uint8_t)((s0p[at] & ~visible) | (s0 & visible));
			s1p[at] = (uint8_t)((s1p[at] & ~visible) | (s1 & visible));
		}
	}
}

/* The dithered or thresholded pixels of a shade-plane word. */
static inline __attribute__((always_inline)) uint32_t
panel_bits(uint32_t s0, uint32_t s1, uint32_t half, uint32_t quarter)
{
	/* Black for shade 3; half the pixels (a checkerboard) for shade 2;
	   a quarter for shade 1 -- or, thresholded, black from shade 2. */
	return dithered ? (s1 & (s0 | half)) | (s0 & ~s1 & quarter) : s1;
}

/* A big-endian word of panel pixels to the framebuffer, as two halfword
   stores: the picture starts at byte 10, which is halfword aligned. */
static inline __attribute__((always_inline)) void
put_word(uint8_t *out, uint32_t black)
{
	uint32_t bytes = __builtin_bswap32(black);	/* leftmost byte first */

	((uint16_t *)out)[0] = (uint16_t)bytes;
	((uint16_t *)out)[1] = (uint16_t)(bytes >> 16);
}

_Static_assert(GBW_LEFT_BYTE % 2 == 0 && GBW_STRIDE % 2 == 0,
	       "put_word stores halfwords");

#ifdef __c33__
/* render_bg.s's arguments; see its header. */
struct bg_args {
	const uint8_t *map, *tiles;
	uint32_t first, flip, shift, identity;
	uint32_t *stream0, *stream1, *plane0, *plane1;
	uint8_t *out;
	uint32_t half, quarter, dithered, output;
	uint32_t masks[8];	/* c0 d10 c2 d32 for shade bit 0, then bit 1 */
	uint32_t *row;		/* map_row: 64 bytes of A0 RAM */
};
void gbw_render_bg(struct bg_args *a);

void gbw_render_dither(struct bg_args *a);

/* The fields that stay put are set once (render_bg_init), the masks when
   BGP changes (render_line), the rest each line. */
static struct bg_args bg_args RENDER_DATA;

static void render_bg_init(void)
{
	bg_args.row = map_row;
	bg_args.stream0 = stream0;
	bg_args.stream1 = stream1;
	bg_args.plane0 = shade0 + 1;
	bg_args.plane1 = shade1 + 1;
}

static inline __attribute__((always_inline)) void render_bg_masks(void)
{
	bg_args.masks[0] = bg_masks.c0_0;
	bg_args.masks[1] = bg_masks.d10_0;
	bg_args.masks[2] = bg_masks.c2_0;
	bg_args.masks[3] = bg_masks.d32_0;
	bg_args.masks[4] = bg_masks.c0_1;
	bg_args.masks[5] = bg_masks.d10_1;
	bg_args.masks[6] = bg_masks.c2_1;
	bg_args.masks[7] = bg_masks.d32_1;
}

static inline __attribute__((always_inline)) void
render_bg(struct gb_s *g, const uint8_t *map, unsigned scx, unsigned tile_row,
	  unsigned lcdc, unsigned ly, uint8_t *out, int sprites)
{
	struct bg_args *a = &bg_args;
	unsigned flip = lcdc & LCDC_TILE_SELECT ? 0 : 0x80;

	a->map = map;
	a->tiles = g->vram + tile_row + (flip ? VRAM_TILES_2 : VRAM_TILES_1);
	a->first = scx >> 3;
	a->flip = flip;
	a->shift = scx & 7;
	a->identity = g->hram_io[IO_BGP] == 0xe4;
	a->out = out;
	a->half = ly & 1 ? 0x55555555u : 0xaaaaaaaau;
	a->quarter = ly & 1 ? 0 : 0xaaaaaaaau;
	a->dithered = dithered;
	a->output = !sprites;
	gbw_render_bg(a);
}
#endif

static RENDER_CODE void render_line(struct gb_s *g)
{
	unsigned ly = g->hram_io[IO_LY];
	unsigned lcdc = g->hram_io[IO_LCDC];
	unsigned bgp = g->hram_io[IO_BGP];
	/* Where the window starts on this line, 160 for nowhere. */
	unsigned window_from = 160, window_bias = 0;
	unsigned bg = 0, bg_shift = 0;
	int sprites = 0;
	uint32_t half = ly & 1 ? 0x55555555u : 0xaaaaaaaau;
	uint32_t quarter = ly & 1 ? 0 : 0xaaaaaaaau;
	uint8_t *out = framebuffer + (GBW_TOP + ly) * GBW_STRIDE + GBW_LEFT_BYTE;

	if (g->direct.frame_skip && !g->display.frame_skip_count)
		return;
#ifdef GBW_CHECK_RENDER
	saved_window_line = g->display.window_clear;
#endif

	if (lcdc & LCDC_WINDOW_ENABLE && ly >= g->display.WY
	    && g->hram_io[IO_WX] <= 166) {
		unsigned wx = g->hram_io[IO_WX];
		unsigned line = g->display.window_clear;
		const uint8_t *map = g->vram
			+ (lcdc & LCDC_WINDOW_MAP ? VRAM_BMAP_2 : VRAM_BMAP_1)
			+ (line >> 3) * 32;

		/* Screen x shows window x + 7 - wx; the stream's blank first
		   word stands for window x -32 to -1. */
		window_bias = 7 - wx + 32;
		window_from = wx < 7 ? 0 : wx - 7;
		window0[0] = window1[0] = 0;
		fetch_stream(map, 0, (line & 7) * 2, lcdc, window0 + 1, window1 + 1);
		g->display.window_clear++;
	}

	if (lcdc & LCDC_OBJ_ENABLE) {
		if (sprites_dirty)
			rebucket(lcdc);
		sprites = line_count[ly] != 0;
	}
	if (bgp != 0xe4 && bgp != bg_masks_for) {
		/* The identity palette needs no masks. */
		palette_masks(bgp, &bg_masks);
		bg_masks_for = bgp;
#ifdef __c33__
		render_bg_masks();
#endif
	}

	if (lcdc & LCDC_BG_ENABLE && window_from > 0) {
		unsigned y = (ly + g->hram_io[IO_SCY]) & 0xff;
		unsigned scx = g->hram_io[IO_SCX];
		const uint8_t *map = g->vram
			+ (lcdc & LCDC_BG_MAP ? VRAM_BMAP_2 : VRAM_BMAP_1)
			+ (y >> 3) * 32;

#ifdef __c33__
		if (window_from == 160) {
			/* The background alone: render_bg.s. */
			render_bg(g, map, scx, (y & 7) * 2, lcdc, ly, out, sprites);
			if (sprites) {
				render_sprites(ly, lcdc);
				gbw_render_dither(&bg_args);
			}
			goto drawn;
		}
#endif
		fetch_stream(map, scx >> 3, (y & 7) * 2, lcdc, stream0, stream1);
		bg = 1;
		bg_shift = scx & 7;
	}


	/* Three passes, each light enough to keep what it needs in the
	   C33's fifteen registers: in one, the compiler spilled. */
	for (unsigned w = 0; w < 5; ++w) {
		uint32_t lo = 0, hi = 0, cover = 0;

		if (bg) {
			lo = funnel(stream0, w, bg_shift);
			hi = funnel(stream1, w, bg_shift);
			cover = 0xffffffffu;
		}
		if (window_from < 32 * w + 32) {
			/* The window's pixels win from window_from on. */
			unsigned at = 32 * w + window_bias;
			uint32_t mask = window_from <= 32 * w ? 0xffffffffu
				: 0xffffffffu >> (window_from - 32 * w);

			lo = (lo & ~mask) | (funnel(window0, at >> 5, at & 31) & mask);
			hi = (hi & ~mask) | (funnel(window1, at >> 5, at & 31) & mask);
			cover |= mask;
		}
		line_lo[w] = lo;
		line_hi[w] = hi;
		line_cover[w] = cover;
	}

	if (bgp == 0xe4) {
		for (unsigned w = 0; w < 5; ++w) {
			shade0[w + 1] = line_lo[w] & line_cover[w];
			shade1[w + 1] = line_hi[w] & line_cover[w];
		}
	} else {
		const struct palette p = bg_masks;

		for (unsigned w = 0; w < 5; ++w) {
			uint32_t lo = line_lo[w], hi = line_hi[w], s0, s1;

			SHADE(p, lo, hi, s0, s1);
			shade0[w + 1] = s0 & line_cover[w];
			shade1[w + 1] = s1 & line_cover[w];
		}
	}

	if (sprites)
		render_sprites(ly, lcdc);
	for (unsigned w = 0; w < 5; ++w)
		put_word(out + 4 * w, panel_bits(shade0[w + 1], shade1[w + 1],
						 half, quarter));
#ifdef __c33__
drawn:
#endif
	++lines_drawn;

#ifdef GBW_CHECK_RENDER
	/* Peanut's renderer on the same line, which calls check_line. */
	{
		uint8_t window = g->display.window_clear;

		g->display.window_clear = saved_window_line;
		__gb_draw_line(g);
		if (g->display.window_clear != window)
			gbw_error("window line differs from Peanut's", ly);
	}
#endif
}
