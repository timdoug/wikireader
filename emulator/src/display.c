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

#include "display.h"


/*
 * A 5x7 bitmap for the handful of characters the button labels need. Not
 * worth a font library: the panel itself comes from the guest's own
 * framebuffer, so this is the only text the emulator draws.
 */
static const struct { char c; const char *rows[7]; } font[] = {
	{ 'R', { "#### ", "#   #", "#   #", "#### ", "#  # ", "#   #", "#   #" } },
	{ 'S', { " ####", "#    ", "#    ", " ### ", "    #", "    #", "#### " } },
	{ 'H', { "#   #", "#   #", "#   #", "#####", "#   #", "#   #", "#   #" } },
	{ '1', { "  #  ", " ##  ", "  #  ", "  #  ", "  #  ", "  #  ", " ### " } },
	{ '2', { " ### ", "#   #", "    #", "   # ", "  #  ", " #   ", "#####" } },
	{ '3', { " ### ", "#   #", "    #", "  ## ", "    #", "#   #", " ### " } },
};

static void draw_char(SDL_Renderer *r, char c, int x, int y, int px)
{
	for (unsigned i = 0; i < sizeof font / sizeof *font; i++) {
		if (font[i].c != c)
			continue;
		for (int row = 0; row < 7; row++)
			for (int col = 0; col < 5; col++)
				if (font[i].rows[row][col] == '#') {
					SDL_Rect q = { x + col * px, y + row * px,
						       px, px };
					SDL_RenderFillRect(r, &q);
				}
		return;
	}
}

/* Where button n sits, in window pixels. */
static SDL_Rect button_rect(const struct display *d, int n)
{
	int gap = BUTTON_GAP;
	int x = gap + n * (BUTTON_W + gap);
	int y = LCD_HEIGHT + (BUTTON_STRIP_H - BUTTON_H) / 2;
	SDL_Rect r = { x * d->scale, y * d->scale,
		       BUTTON_W * d->scale, BUTTON_H * d->scale };
	return r;
}

/* Which button a window-pixel lands on, or -1. */
static int button_hit(const struct display *d, int wx, int wy)
{
	for (int n = 0; n < 3; n++) {
		SDL_Rect r = button_rect(d, n);
		if (wx >= r.x && wx < r.x + r.w && wy >= r.y && wy < r.y + r.h)
			return n;
	}
	return -1;
}

static void draw_buttons(struct display *d)
{
	static const char label[3][2] = { {'1','R'}, {'2','S'}, {'3','H'} };

	SDL_SetRenderDrawColor(d->renderer, 0x20, 0x20, 0x20, 0xff);
	SDL_Rect strip = { 0, LCD_HEIGHT * d->scale,
			   LCD_WIDTH * d->scale, BUTTON_STRIP_H * d->scale };
	SDL_RenderFillRect(d->renderer, &strip);

	for (int n = 0; n < 3; n++) {
		SDL_Rect r = button_rect(d, n);
		bool down = (d->button_held == n);
		SDL_SetRenderDrawColor(d->renderer,
				       down ? 0xff : 0x60,
				       down ? 0xcc : 0x60,
				       down ? 0x33 : 0x60, 0xff);
		SDL_RenderFillRect(d->renderer, &r);

		int px = d->scale > 1 ? d->scale - 1 : 1;
		int tw = (5 + 2 + 5) * px;
		int tx = r.x + (r.w - tw) / 2;
		int ty = r.y + (r.h - 7 * px) / 2;
		SDL_SetRenderDrawColor(d->renderer,
				       down ? 0x00 : 0xdd,
				       down ? 0x00 : 0xdd,
				       down ? 0x00 : 0xdd, 0xff);
		draw_char(d->renderer, label[n][0], tx, ty, px);
		draw_char(d->renderer, label[n][1], tx + 7 * px, ty, px);
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

	d->window = SDL_CreateWindow("WikiReader",
				     SDL_WINDOWPOS_CENTERED,
				     SDL_WINDOWPOS_CENTERED,
				     LCD_WIDTH * d->scale,
				     (LCD_HEIGHT + BUTTON_STRIP_H) * d->scale, 0);
	if (!d->window)
		return false;

	d->renderer = SDL_CreateRenderer(d->window, -1,
					 SDL_RENDERER_ACCELERATED);
	if (!d->renderer)
		d->renderer = SDL_CreateRenderer(d->window, -1, 0);
	if (!d->renderer)
		return false;

	d->texture = SDL_CreateTexture(d->renderer, SDL_PIXELFORMAT_ARGB8888,
				       SDL_TEXTUREACCESS_STREAMING,
				       LCD_WIDTH, LCD_HEIGHT);
	if (!d->texture)
		return false;

	d->button = -1;
	d->button_held = -1;
	d->open = true;
	return true;
}

void display_close(struct display *d)
{
	if (!d->open)
		return;
	SDL_DestroyTexture(d->texture);
	SDL_DestroyRenderer(d->renderer);
	SDL_DestroyWindow(d->window);
	SDL_Quit();
	d->open = false;
}

/* Pump events and repaint. Returns false once the user closes the window. */
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
			if (ev.key.keysym.sym >= SDLK_1 && ev.key.keysym.sym <= SDLK_3) {
				d->button = ev.key.keysym.sym - SDLK_1;
				d->button_pressed = (ev.type == SDL_KEYDOWN);
			}
			break;
		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEBUTTONUP: {
			int hit = button_hit(d, ev.button.x, ev.button.y);
			if (ev.type == SDL_MOUSEBUTTONDOWN && hit >= 0) {
				d->button = hit;
				d->button_pressed = true;
				d->button_held = hit;
				break;
			}
			if (ev.type == SDL_MOUSEBUTTONUP && d->button_held >= 0) {
				d->button = d->button_held;
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

	uint32_t *pixels;
	int pitch;
	if (SDL_LockTexture(d->texture, NULL, (void **)&pixels, &pitch) != 0)
		return true;

	for (int y = 0; y < LCD_HEIGHT; y++) {
		uint32_t *row = (uint32_t *)((uint8_t *)pixels + y * pitch);
		for (int x = 0; x < LCD_WIDTH; x++)
			row[x] = lcd_pixel(d->lcd, d->mem, x, y)
				 ? 0xff000000u : 0xffffffffu;
	}
	SDL_UnlockTexture(d->texture);

	SDL_RenderClear(d->renderer);
	SDL_Rect panel = { 0, 0, LCD_WIDTH * d->scale, LCD_HEIGHT * d->scale };
	SDL_RenderCopy(d->renderer, d->texture, NULL, &panel);
	draw_buttons(d);
	SDL_RenderPresent(d->renderer);
	return true;
}
