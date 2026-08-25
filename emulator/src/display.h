#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

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
	unsigned last_present_ms;  /* repaints are paced to the display, not the guest */
	/*
	 * The bezel is hundreds of small draw calls and almost never changes,
	 * so it is rendered once into a texture and blitted after that.
	 */
	struct SDL_Texture *bezel_tex;
	int   bezel_drawn_held;    /* button_held the texture was drawn for */
	uint64_t last_fingerprint;
	bool  have_fingerprint;
	bool  powered;           /* false blanks the panel, as an off LCD is */
	/*
	 * Count of power-switch press edges, never reset here. A press and
	 * its release can both arrive in one poll, which leaves button_pressed
	 * false and loses the press; while the device is off that is the one
	 * event that has to survive, so it is counted rather than sampled.
	 */
	unsigned power_presses;
	unsigned long presents, skipped, calls;
};

bool display_open(struct display *d, struct lcd *lcd, struct mem *mem, int scale);
void display_close(struct display *d);
bool display_update(struct display *d);
/* Apply a single SDL event; exposed for tools/test_display.c. */
union SDL_Event;
void display_handle_event(struct display *d, const union SDL_Event *ev);
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
#define BUTTON_CX0      88          /* centre of the leftmost button */
#define BUTTON_DX       52          /* spacing between centres */

/*
 * The power switch is on the edge of the case, not the bezel, so it is
 * drawn small and set apart from the three the case actually prints.
 */
#define POWER_CX        226
#define POWER_R         11

#endif /* DISPLAY_H */
