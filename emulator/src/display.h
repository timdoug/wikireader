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
};

bool display_open(struct display *d, struct lcd *lcd, struct mem *mem, int scale);
void display_close(struct display *d);
bool display_update(struct display *d);

#endif /* DISPLAY_H */
