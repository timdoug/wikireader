/*
 * Window event handling.
 *
 * The panel and the buttons are the parts of the emulator a scripted run
 * never touches: -T and -G call touch_post() directly, so everything
 * between an SDL event and that call went untested. Both bugs this code
 * has had lived in that gap -- a power press handed to a machine that was
 * not running, and a drag that let go outside the panel and never lifted
 * the finger.
 *
 * display_handle_event() is pure state, so it can be driven straight from
 * here with no window and no SDL video device.
 */
#include <SDL.h>
#include <stdio.h>
#include <string.h>

#include "../src/display.h"

static int failures;

static void ok(const char *what, bool cond)
{
	printf("%-62s %s\n", what, cond ? "ok" : "FAILED");
	if (!cond)
		failures++;
}

static struct display fresh(void)
{
	struct display d;
	memset(&d, 0, sizeof d);
	d.open = true;
	d.scale = 3;
	d.button = -1;
	d.button_held = -1;
	return d;
}

static SDL_Event mouse(uint32_t type, int x, int y)
{
	SDL_Event e;
	memset(&e, 0, sizeof e);
	e.type = type;
	e.button.x = x;
	e.button.y = y;
	return e;
}

static SDL_Event motion(int x, int y)
{
	SDL_Event e;
	memset(&e, 0, sizeof e);
	e.type = SDL_MOUSEMOTION;
	e.motion.x = x;
	e.motion.y = y;
	return e;
}

static SDL_Event key(uint32_t type, SDL_Keycode sym)
{
	SDL_Event e;
	memset(&e, 0, sizeof e);
	e.type = type;
	e.key.keysym.sym = sym;
	return e;
}

int main(void)
{
	const int sc = 3;
	const int below = LCD_HEIGHT * sc + 20;   /* on the bezel */

	/*
	 * The reported bug. Scrolling the text up means dragging downwards,
	 * so the button that started on the panel is released past the
	 * bottom edge, over the bezel. The release has to lift the finger
	 * anyway or the panel stays down and the scroll keeps the drag
	 * instead of coasting.
	 */
	{
		struct display d = fresh();
		SDL_Event e;

		e = mouse(SDL_MOUSEBUTTONDOWN, 100 * sc, 60 * sc);
		display_handle_event(&d, &e);
		ok("press on the panel starts a touch", d.touch_pressed);

		e = motion(100 * sc, below);
		display_handle_event(&d, &e);
		ok("dragging past the bottom keeps the finger down",
		   d.touch_pressed);
		ok("and reports the edge, not a coordinate off the glass",
		   d.touch_y == LCD_HEIGHT - 1);

		d.touch_pending = false;
		e = mouse(SDL_MOUSEBUTTONUP, 100 * sc, below);
		display_handle_event(&d, &e);
		ok("releasing over the bezel still lifts the finger",
		   !d.touch_pressed);
		ok("and the release is queued for the panel", d.touch_pending);
	}

	/* The direction that always worked: dragging up leaves the top edge. */
	{
		struct display d = fresh();
		SDL_Event e;

		e = mouse(SDL_MOUSEBUTTONDOWN, 100 * sc, 60 * sc);
		display_handle_event(&d, &e);
		e = mouse(SDL_MOUSEBUTTONUP, 100 * sc, -40);
		display_handle_event(&d, &e);
		ok("releasing above the panel lifts the finger", !d.touch_pressed);
		ok("clamped to the top row", d.touch_y == 0);
	}

	/* Sideways overshoot has the same problem in x. */
	{
		struct display d = fresh();
		SDL_Event e;

		e = mouse(SDL_MOUSEBUTTONDOWN, 100 * sc, 60 * sc);
		display_handle_event(&d, &e);
		e = motion(-30, 60 * sc);
		display_handle_event(&d, &e);
		ok("dragging off the left edge clamps to column 0", d.touch_x == 0);
		e = motion((LCD_WIDTH + 50) * sc, 60 * sc);
		display_handle_event(&d, &e);
		ok("and off the right edge to the last column",
		   d.touch_x == LCD_WIDTH - 1);
	}

	/* A press on the bezel is a button, not a touch. */
	{
		struct display d = fresh();
		SDL_Event e = mouse(SDL_MOUSEBUTTONDOWN, 100 * sc, below);
		display_handle_event(&d, &e);
		ok("a press on the bezel does not touch the panel",
		   !d.touch_pressed && !d.touch_pending);
	}

	/* A stray release with no finger down must not invent one. */
	{
		struct display d = fresh();
		SDL_Event e = mouse(SDL_MOUSEBUTTONUP, 100 * sc, 60 * sc);
		display_handle_event(&d, &e);
		ok("a release with no touch in progress is ignored",
		   !d.touch_pending);
	}

	/*
	 * Power presses are counted, not sampled: a quick click delivers its
	 * down and up in one poll, and sampling button_pressed afterwards
	 * sees only the up. While the device is off that press is the one
	 * event that cannot be missed.
	 */
	{
		struct display d = fresh();
		SDL_Event down = key(SDL_KEYDOWN, SDLK_p);
		SDL_Event up   = key(SDL_KEYUP, SDLK_p);

		display_handle_event(&d, &down);
		display_handle_event(&d, &up);
		ok("a press and release in one poll still counts one press",
		   d.power_presses == 1);
		ok("and sampling alone would have missed it", !d.button_pressed);

		display_handle_event(&d, &down);
		display_handle_event(&d, &up);
		ok("a second press counts again", d.power_presses == 2);
	}

	if (failures == 0)
		printf("all display tests passed\n");
	else
		printf("%d display test(s) FAILED\n", failures);
	return failures != 0;
}
