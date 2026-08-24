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
				     LCD_HEIGHT * d->scale, 0);
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
			if (ev.key.keysym.sym == SDLK_ESCAPE ||
			    ev.key.keysym.sym == SDLK_q)
				d->quit = true;
			break;
		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEBUTTONUP:
			/* Queued for the touch panel; see touch_post(). */
			d->touch_x = ev.button.x / d->scale;
			d->touch_y = ev.button.y / d->scale;
			d->touch_pressed = (ev.type == SDL_MOUSEBUTTONDOWN);
			d->touch_pending = true;
			break;
		}
	}
	if (d->quit)
		return false;

	uint32_t *pixels;
	int pitch;
	if (SDL_LockTexture(d->texture, NULL, (void **)&pixels, &pitch) != 0)
		return true;

	uint32_t base = d->lcd->fb_addr;
	for (int y = 0; y < LCD_HEIGHT; y++) {
		uint32_t *row = (uint32_t *)((uint8_t *)pixels + y * pitch);
		for (int x = 0; x < LCD_WIDTH; x++) {
			unsigned b = mem_read(d->mem,
					      base + y * LCD_STRIDE + (x >> 3), 1);
			unsigned on = (b >> (7 - (x & 7))) & 1;
			row[x] = on ? 0xff000000u : 0xffffffffu;
		}
	}
	SDL_UnlockTexture(d->texture);

	SDL_RenderClear(d->renderer);
	SDL_RenderCopy(d->renderer, d->texture, NULL, NULL);
	SDL_RenderPresent(d->renderer);
	return true;
}
