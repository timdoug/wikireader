#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#include "lcd.h"
#include "mem.h"

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;

struct display {
	struct SDL_Window   *window;
	struct SDL_Renderer *renderer;
	struct SDL_Texture  *texture;
	struct lcd *lcd;
	struct mem *mem;
	int   scale;
	bool  open;
	bool  quit;

	/* most recent mouse event, consumed by the touch panel */
	int   touch_x, touch_y;
	bool  touch_pressed;
	bool  touch_pending;
	/* front buttons: 0 random, 1 search, 2 history */
	int   button;            /* -1 when nothing to report */
	bool  button_pressed;
	int   button_held;       /* -1, or the button currently drawn pressed */
};

bool display_open(struct display *d, struct lcd *lcd, struct mem *mem, int scale);
void display_close(struct display *d);
bool display_update(struct display *d);
/* Give the host CPU back for a few milliseconds while the guest idles. */
void display_idle_wait(struct display *d, unsigned ms);

/*
 * The bezel below the panel, laid out like the device: a WikiReader
 * wordmark on the left, then three round buttons reading search, history,
 * random from left to right. Sized in panel pixels and scaled with
 * everything else.
 *
 * Note the order. grifo numbers them 0 random, 1 search, 2 history
 * (button.c), which is not the order they sit in on the case.
 */
#define BEZEL_H         46
#define BUTTON_R        19          /* radius */
#define BUTTON_CY       (LCD_HEIGHT + BEZEL_H / 2)
#define BUTTON_CX0      92          /* centre of the leftmost button */
#define BUTTON_DX       56          /* spacing between centres */

#endif /* DISPLAY_H */
