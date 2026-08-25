/*
 * SDL2 window showing the emulated panel live.
 *
 * The framebuffer is 240x208 at 1 bit per pixel with rows padded to 32 bytes
 * (grifo.lds: __BYTES_FrameBuffer = (256/8)*208). A set bit is a lit pixel,
 * drawn black on the real monochrome panel.
 *
 * SDL_MAIN_HANDLED keeps SDL from redefining main(); we call
 * SDL_SetMainReady() ourselves instead.
 */

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "display.h"


/*
 * A 3x5 bitmap for the letters the bezel needs. The panel itself is drawn
 * from the guest's framebuffer, so this is the only text the emulator
 * renders, and it is not worth a font library. The case prints the labels
 * in lower case; these are small capitals, which read the same at this
 * size.
 */
static const struct { char c; const char *rows[5]; } font3x5[] = {
	{ 'A', { ".#.", "#.#", "###", "#.#", "#.#" } },
	{ 'C', { ".##", "#..", "#..", "#..", ".##" } },
	{ 'D', { "##.", "#.#", "#.#", "#.#", "##." } },
	{ 'E', { "###", "#..", "##.", "#..", "###" } },
	{ 'H', { "#.#", "#.#", "###", "#.#", "#.#" } },
	{ 'I', { "###", ".#.", ".#.", ".#.", "###" } },
	{ 'K', { "#.#", "#.#", "##.", "#.#", "#.#" } },
	{ 'M', { "#.#", "###", "###", "#.#", "#.#" } },
	{ 'N', { "#.#", "###", "###", "###", "#.#" } },
	{ 'O', { ".#.", "#.#", "#.#", "#.#", ".#." } },
	{ 'R', { "##.", "#.#", "##.", "#.#", "#.#" } },
	{ 'S', { ".##", "#..", ".#.", "..#", "##." } },
	{ 'T', { "###", ".#.", ".#.", ".#.", ".#." } },
	{ 'W', { "#.#", "#.#", "###", "###", "#.#" } },
	{ 'Y', { "#.#", "#.#", ".#.", ".#.", ".#." } },
};

static void draw_char(SDL_Renderer *r, char c, int x, int y, int px)
{
	for (unsigned i = 0; i < sizeof font3x5 / sizeof *font3x5; i++) {
		if (font3x5[i].c != c)
			continue;
		for (int row = 0; row < 5; row++)
			for (int col = 0; col < 3; col++)
				if (font3x5[i].rows[row][col] == '#') {
					SDL_Rect q = { x + col * px, y + row * px,
						       px, px };
					SDL_RenderFillRect(r, &q);
				}
		return;
	}
}

static int text_width(const char *s, int px) { return (int)strlen(s) * 4 * px; }

static void draw_text(SDL_Renderer *r, const char *s, int x, int y, int px)
{
	for (; *s; s++, x += 4 * px)
		draw_char(r, *s, x, y, px);
}

/* Filled or outlined circle, drawn as horizontal spans. */
static void draw_circle(SDL_Renderer *r, int cx, int cy, int rad, bool filled,
			int thick)
{
	for (int dy = -rad; dy <= rad; dy++) {
		int dx = (int)(sqrt((double)rad * rad - (double)dy * dy) + 0.5);
		if (filled) {
			SDL_RenderDrawLine(r, cx - dx, cy + dy, cx + dx, cy + dy);
			continue;
		}
		int inner = rad - thick;
		int ix = 0;
		if (inner > 0 && dy > -inner && dy < inner)
			ix = (int)(sqrt((double)inner * inner -
					(double)dy * dy) + 0.5);
		if (ix == 0) {
			SDL_RenderDrawLine(r, cx - dx, cy + dy, cx + dx, cy + dy);
		} else {
			SDL_RenderDrawLine(r, cx - dx, cy + dy, cx - ix, cy + dy);
			SDL_RenderDrawLine(r, cx + ix, cy + dy, cx + dx, cy + dy);
		}
	}
}

/*
 * Buttons left to right as they are on the case, with the code grifo uses
 * for each: search is 1, history 2, random 0.
 */
static const struct { const char *label; int code; } buttons[4] = {
	{ "SEARCH",  1 },
	{ "HISTORY", 2 },
	{ "RANDOM",  0 },
	{ "",        3 },   /* power, drawn as a symbol rather than a word */
};

/* Centre within the window. */
static void button_centre(const struct display *d, int n, int *cx, int *cy)
{
	*cx = (BUTTON_CX0 + n * BUTTON_DX) * d->scale;
	*cy = BUTTON_CY * d->scale;
}

/* Centre within the bezel texture, whose origin is the top of the strip. */
static void button_centre_local(const struct display *d, int n, int *cx, int *cy)
{
	*cx = (BUTTON_CX0 + n * BUTTON_DX) * d->scale;
	*cy = (BEZEL_H / 2) * d->scale;
}

/* Centre of the power control, in window pixels. */
static void power_centre(const struct display *d, int *cx, int *cy)
{
	*cx = POWER_CX * d->scale;
	*cy = BUTTON_CY * d->scale;
}

/* Which button a window-pixel lands on, or -1. Index 3 is power. */
static int button_hit(const struct display *d, int wx, int wy)
{
	{
		int cx, cy;
		power_centre(d, &cx, &cy);
		int rad = POWER_R * d->scale;
		int dx = wx - cx, dy = wy - cy;
		if (dx * dx + dy * dy <= rad * rad)
			return 3;
	}
	for (int n = 0; n < 3; n++) {
		int cx, cy;
		button_centre(d, n, &cx, &cy);
		int rad = BUTTON_R * d->scale;
		int dx = wx - cx, dy = wy - cy;
		if (dx * dx + dy * dy <= rad * rad)
			return n;
	}
	return -1;
}

/* Draw the bezel into the current render target. */
static void paint_bezel(struct display *d)
{
	SDL_SetRenderDrawColor(d->renderer, 0x10, 0x10, 0x10, 0xff);
	SDL_Rect strip = { 0, 0, LCD_WIDTH * d->scale, BEZEL_H * d->scale };
	SDL_RenderFillRect(d->renderer, &strip);

	/*
	 * Label size is tied to the scale so the text keeps its proportion
	 * inside the circles: at one pixel per scale step, the longest label
	 * spans about three quarters of a button's diameter.
	 */
	int px = d->scale;

	/* wordmark, as on the case */
	SDL_SetRenderDrawColor(d->renderer, 0xff, 0xff, 0xff, 0xff);
	draw_text(d->renderer, "WIKIREADER", 8 * d->scale,
		  ((BEZEL_H / 2) * d->scale) - 2 * px, px);

	for (int n = 0; n < 3; n++) {
		int cx, cy;
		button_centre_local(d, n, &cx, &cy);
		int rad = BUTTON_R * d->scale;
		bool down = (d->button_held == n);

		SDL_SetRenderDrawColor(d->renderer, 0xff, 0xff, 0xff, 0xff);
		draw_circle(d->renderer, cx, cy, rad, down, d->scale);

		int lpx = px;
		int tw = text_width(buttons[n].label, lpx);
		if (down)
			SDL_SetRenderDrawColor(d->renderer, 0x10, 0x10, 0x10, 0xff);
		draw_text(d->renderer, buttons[n].label,
			  cx - tw / 2, cy - 2 * lpx, lpx);
	}

	/*
	 * Power, as the usual broken ring and stem. Smaller than the others
	 * and off to one side, because on the case it is not one of them.
	 */
	{
		int cx = POWER_CX * d->scale;
		int cy = (BEZEL_H / 2) * d->scale;
		int rad = POWER_R * d->scale;
		bool down = (d->button_held == 3);
		int t = d->scale > 1 ? d->scale : 1;

		SDL_SetRenderDrawColor(d->renderer, 0xff, 0xff, 0xff, 0xff);
		draw_circle(d->renderer, cx, cy, rad, down, t);

		/* ring */
		SDL_SetRenderDrawColor(d->renderer,
				       down ? 0x10 : 0xff, down ? 0x10 : 0xff,
				       down ? 0x10 : 0xff, 0xff);
		draw_circle(d->renderer, cx, cy, rad / 2, false, t);
		/* gap at the top, then the stem through it */
		SDL_SetRenderDrawColor(d->renderer,
				       down ? 0xff : 0x10, down ? 0xff : 0x10,
				       down ? 0xff : 0x10, 0xff);
		SDL_Rect gap = { cx - t, cy - rad / 2 - t, 2 * t, t * 3 };
		SDL_RenderFillRect(d->renderer, &gap);
		SDL_SetRenderDrawColor(d->renderer,
				       down ? 0x10 : 0xff, down ? 0x10 : 0xff,
				       down ? 0x10 : 0xff, 0xff);
		SDL_Rect stem = { cx - t / 2 - (t > 1), cy - rad / 2 - t,
				  t + 2 * (t > 1), rad / 2 + t };
		SDL_RenderFillRect(d->renderer, &stem);
	}
}

bool display_open(struct display *d, struct lcd *lcd, struct mem *mem,
		  int scale)
{
	memset(d, 0, sizeof *d);
	d->lcd = lcd;
	d->mem = mem;
	d->scale = scale > 0 ? scale : 3;

	SDL_SetMainReady();
	if (SDL_Init(SDL_INIT_VIDEO) != 0)
		return false;

	/*
	 * Nearest-neighbour scaling. The panel is one bit per pixel; every
	 * pixel is meant to be a hard square, and interpolating between them
	 * is just blur.
	 */
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

	d->window = SDL_CreateWindow("WikiReader",
				     SDL_WINDOWPOS_CENTERED,
				     SDL_WINDOWPOS_CENTERED,
				     LCD_WIDTH * d->scale,
				     (LCD_HEIGHT + BEZEL_H) * d->scale,
				     SDL_WINDOW_ALLOW_HIGHDPI);
	if (!d->window)
		return false;

	/*
	 * Present in step with the display.
	 *
	 * Without this the emulator hands over a new frame whenever it has
	 * one, and the display scans out part of the old buffer and part of
	 * the new -- a thin seam along an edge. It shows only while frames
	 * are actually being presented, so it appears when the guest is
	 * drawing and vanishes the moment the screen settles, and it cannot
	 * be screenshotted at all: a screenshot copies the composited
	 * surface, which is intact, while tearing happens later, during
	 * scanout.
	 *
	 * The cost is that a present waits for the next refresh, which is
	 * the right thing for a window: it is exactly the pacing a display
	 * can use. Frames that change nothing are skipped before we get
	 * here, so an idle screen never waits at all.
	 */
	d->renderer = SDL_CreateRenderer(d->window, -1,
					 SDL_RENDERER_ACCELERATED |
					 SDL_RENDERER_PRESENTVSYNC);
	if (!d->renderer)
		d->renderer = SDL_CreateRenderer(d->window, -1,
						 SDL_RENDERER_PRESENTVSYNC);
	if (!d->renderer)
		d->renderer = SDL_CreateRenderer(d->window, -1, 0);
	if (!d->renderer)
		return false;

	/*
	 * Ask for a high-DPI drawable and then draw in the logical size we
	 * already use. Without this the backing store is at window size and
	 * the compositor upscales it to a Retina panel, which looks soft --
	 * most obviously on a still screen, where there is time to notice.
	 * With it, SDL does the scaling itself, and at an integer factor
	 * with nearest-neighbour it stays sharp.
	 */
	SDL_RenderSetLogicalSize(d->renderer, LCD_WIDTH * d->scale,
				 (LCD_HEIGHT + BEZEL_H) * d->scale);

	d->texture = SDL_CreateTexture(d->renderer, SDL_PIXELFORMAT_ARGB8888,
				       SDL_TEXTUREACCESS_STREAMING,
				       LCD_WIDTH, LCD_HEIGHT);
	if (!d->texture)
		return false;

	d->button = -1;
	d->button_held = -1;
	d->powered = false;
	d->power_presses = 0;
	d->bezel_tex = SDL_CreateTexture(d->renderer, SDL_PIXELFORMAT_ARGB8888,
					 SDL_TEXTUREACCESS_TARGET,
					 LCD_WIDTH * d->scale,
					 BEZEL_H * d->scale);
	d->bezel_drawn_held = -2;      /* force the first paint */
	if (getenv("WREMU_DISPLAY_INFO")) {
		int ww, wh, dw, dh, ow, oh;
		float sx, sy;
		SDL_GetWindowSize(d->window, &ww, &wh);
		SDL_GL_GetDrawableSize(d->window, &dw, &dh);
		SDL_GetRendererOutputSize(d->renderer, &ow, &oh);
		SDL_RenderGetScale(d->renderer, &sx, &sy);
		SDL_Rect vp;
		SDL_RenderGetViewport(d->renderer, &vp);
		fprintf(stderr,
			"display: window %dx%d drawable %dx%d output %dx%d "
			"scale %.4gx%.4g viewport %d,%d %dx%d logical %dx%d\n",
			ww, wh, dw, dh, ow, oh, sx, sy,
			vp.x, vp.y, vp.w, vp.h,
			LCD_WIDTH * d->scale, (LCD_HEIGHT + BEZEL_H) * d->scale);
	}

	d->open = true;
	return true;
}

void display_idle_wait(struct display *d, unsigned ms)
{
	(void)d;
	SDL_Delay(ms);
}

void display_close(struct display *d)
{
	if (!d->open)
		return;
	if (d->bezel_tex)
		SDL_DestroyTexture(d->bezel_tex);
	SDL_DestroyTexture(d->texture);
	SDL_DestroyRenderer(d->renderer);
	SDL_DestroyWindow(d->window);
	SDL_Quit();
	d->open = false;
}

/* Pump events and repaint. Returns false once the user closes the window. */
/*
 * Blit the bezel, re-rendering it only when a button changes state. Drawn
 * live it was several hundred draw calls a frame -- every lit glyph pixel
 * is its own rectangle -- for something static, and the compositor was
 * doing more work than the emulator.
 */
static void draw_bezel(struct display *d)
{
	if (!d->bezel_tex)
		return;
	if (d->bezel_drawn_held != d->button_held) {
		SDL_SetRenderTarget(d->renderer, d->bezel_tex);
		paint_bezel(d);
		SDL_SetRenderTarget(d->renderer, NULL);
		d->bezel_drawn_held = d->button_held;
	}
	SDL_Rect dst = { 0, LCD_HEIGHT * d->scale,
			 LCD_WIDTH * d->scale, BEZEL_H * d->scale };
	SDL_RenderCopy(d->renderer, d->bezel_tex, NULL, &dst);
}

bool display_update(struct display *d)
{
	if (!d->open)
		return true;

	SDL_Event ev;
	while (SDL_PollEvent(&ev)) {
		switch (ev.type) {
		case SDL_QUIT:
			d->quit = true;
			break;
		case SDL_KEYDOWN:
		case SDL_KEYUP:
			if (ev.type == SDL_KEYDOWN &&
			    (ev.key.keysym.sym == SDLK_ESCAPE ||
			     ev.key.keysym.sym == SDLK_q)) {
				d->quit = true;
				break;
			}
			/*
			 * The three front buttons, on keys 1, 2 and 3. grifo
			 * calls them random, search and history, and reports
			 * them in that order (button.c: "0=random, 1=search,
			 * 2=history").
			 */
			/* The power switch is on the case, not the bezel. */
			if (ev.key.keysym.sym == SDLK_p) {
				d->button = 3;
				d->button_pressed = (ev.type == SDL_KEYDOWN);
				if (ev.type == SDL_KEYDOWN && !ev.key.repeat)
					d->power_presses++;
				break;
			}
			if (ev.key.keysym.sym >= SDLK_1 && ev.key.keysym.sym <= SDLK_3) {
				d->button = buttons[ev.key.keysym.sym - SDLK_1].code;
				d->button_pressed = (ev.type == SDL_KEYDOWN);
			}
			break;
		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEBUTTONUP: {
			int hit = button_hit(d, ev.button.x, ev.button.y);
			if (ev.type == SDL_MOUSEBUTTONDOWN && hit >= 0) {
				d->button = buttons[hit].code;
				d->button_pressed = true;
				d->button_held = hit;
				if (buttons[hit].code == 3)
					d->power_presses++;
				break;
			}
			if (ev.type == SDL_MOUSEBUTTONUP && d->button_held >= 0) {
				d->button = buttons[d->button_held].code;
				d->button_pressed = false;
				d->button_held = -1;
				break;
			}
			/* Below the panel but not on a button: not a touch. */
			if (ev.button.y >= LCD_HEIGHT * d->scale)
				break;
			/* Queued for the touch panel; see touch_post(). */
			d->touch_x = ev.button.x / d->scale;
			d->touch_y = ev.button.y / d->scale;
			d->touch_pressed = (ev.type == SDL_MOUSEBUTTONDOWN);
			d->touch_pending = true;
			/*
			 * Keep receiving motion and, crucially, the release
			 * even after the pointer leaves the window, so a drag
			 * that overshoots the edge still ends properly instead
			 * of leaving the panel stuck down.
			 */
			SDL_CaptureMouse(d->touch_pressed ? SDL_TRUE : SDL_FALSE);
			break;
		}
		case SDL_MOUSEMOTION:
			/*
			 * The panel only reports while it is being touched, so
			 * motion with no button held is not an event. Dragging
			 * is what produces EVENT_TOUCH_MOTION in grifo: its CTP
			 * driver emits DOWN for the first pressed packet and
			 * MOTION for every one after it, so a drag has to be a
			 * run of pressed packets with changing coordinates.
			 */
			if (d->touch_pressed) {
				d->touch_x = ev.motion.x / d->scale;
				d->touch_y = ev.motion.y / d->scale;
				d->touch_pending = true;
			}
			break;
		}
	}
	if (d->quit)
		return false;

	/*
	 * Pump events every call, but bound how often the rest of this runs.
	 *
	 * It used to be driven purely by guest cycles -- once every 200k --
	 * which at emulation speed is several hundred frames a second, and
	 * the cost landed on the compositor rather than showing up as the
	 * emulator's own CPU.
	 *
	 * vsync now does the pacing, so this is no longer a frame limiter;
	 * its job is to bound the fingerprint below, which reads the whole
	 * framebuffer. Deliberately looser than the refresh rate: throttling
	 * at exactly 60 Hz against a 60 Hz display, with only millisecond
	 * resolution to work with, means sometimes just missing a refresh
	 * and waiting for the next -- a beat that shows as judder. Letting
	 * frames through faster than the display costs nothing, because
	 * vsync is what decides when they actually go out.
	 */
	d->calls++;
	unsigned now_ms = SDL_GetTicks();
	if (now_ms - d->last_present_ms < 1000 / 120)
		return true;
	d->last_present_ms = now_ms;

	/*
	 * Nothing draws to an idle panel, so most of these frames would be
	 * identical. Hashing the framebuffer is far cheaper than uploading
	 * and presenting it.
	 */
	uint64_t fp = d->powered ? lcd_fingerprint(d->lcd, d->mem) : 0;
	if (d->have_fingerprint && fp == d->last_fingerprint &&
	    d->bezel_drawn_held == d->button_held) {
		d->skipped++;
		return true;
	}
	d->last_fingerprint = fp;
	d->have_fingerprint = true;

	uint32_t *pixels;
	int pitch;
	if (SDL_LockTexture(d->texture, NULL, (void **)&pixels, &pitch) != 0)
		return true;

	for (int y = 0; y < LCD_HEIGHT; y++) {
		uint32_t *row = (uint32_t *)((uint8_t *)pixels + y * pitch);
		for (int x = 0; x < LCD_WIDTH; x++)
			/*
			 * An unpowered panel is not white, it is the blank
			 * grey of an LCD with nothing driving it.
			 */
			row[x] = !d->powered ? 0xffb8b8b0u
			       : lcd_pixel(d->lcd, d->mem, x, y)
				 ? 0xff000000u : 0xffffffffu;
	}
	SDL_UnlockTexture(d->texture);

	/*
	 * Set the clear colour rather than inheriting it. SDL_RenderClear
	 * uses whatever colour was last set, which here was left over from
	 * painting the bezel -- white, from the labels. The panel and bezel
	 * between them cover the whole logical area, so it should never
	 * show; but with a scaled drawable it can catch a pixel at the
	 * edges, and with double buffering the two back buffers disagreed
	 * about it. That is a thin light border that flickers while frames
	 * are being presented and freezes to one state when they stop.
	 */
	SDL_SetRenderDrawColor(d->renderer, 0x00, 0x00, 0x00, 0xff);
	SDL_RenderClear(d->renderer);
	SDL_Rect panel = { 0, 0, LCD_WIDTH * d->scale, LCD_HEIGHT * d->scale };
	SDL_RenderCopy(d->renderer, d->texture, NULL, &panel);
	draw_bezel(d);
	/*
	 * Optional capture of exactly what is about to be shown, for
	 * checking edges. Must happen before the present: after it the back
	 * buffer contents are undefined.
	 */
	if (getenv("WREMU_GRAB")) {
		static int grab_n;
		char path[256];
		snprintf(path, sizeof path, "%s.%03d.ppm",
			 getenv("WREMU_GRAB"), grab_n++);
		int ow, oh;
		SDL_GetRendererOutputSize(d->renderer, &ow, &oh);
		uint32_t *px = malloc((size_t)ow * oh * 4);
		if (px && SDL_RenderReadPixels(d->renderer, NULL,
					       SDL_PIXELFORMAT_ARGB8888,
					       px, ow * 4) == 0) {
			FILE *fp = fopen(path, "wb");
			if (fp) {
				fprintf(fp, "P6\n%d %d\n255\n", ow, oh);
				for (int i = 0; i < ow * oh; i++) {
					uint32_t v = px[i];
					fputc((v >> 16) & 0xff, fp);
					fputc((v >> 8) & 0xff, fp);
					fputc(v & 0xff, fp);
				}
				fclose(fp);
			}
		}
		free(px);
	}

	SDL_RenderPresent(d->renderer);
	d->presents++;
	return true;
}
