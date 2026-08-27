/*
 * Copyright (c) 2009 Openmoko Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef WIKILIB_H
#define WIKILIB_H

#ifndef __cplusplus
// for size_t / ssize_t
#include <stddef.h>
#include <stdbool.h>
#endif
#include "keyboard.h"

#ifndef NULL
#define NULL 0
#endif

#ifndef MIN
#define MIN(a,b) ((a) > (b) ? (b) : (a))
#endif

#ifndef MAX
#define MAX(a,b) ((a) < (b) ? (b) : (a))
#endif

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof((a)) / sizeof((a)[0]))
#endif

#ifndef STATIC_ASSERT
#define STATIC_ASSERT(exp, name) typedef int dummy##name [(exp) ? 1 : -1];
#endif

#define ARTICLE_NEW		0
#define ARTICLE_HISTORY		1
#define ARTICLE_BROWSE		2
#define PHONE_STYLE_KEYIN_BEFORE_COMMIT_TIME 1.5

enum display_mode_e {

	DISPLAY_MODE_INDEX,
	DISPLAY_MODE_ARTICLE,
	DISPLAY_MODE_HISTORY,
	DISPLAY_MODE_RESTRICTED,
	DISPLAY_MODE_WIKI_SELECTION,
};

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned long u32;

typedef signed char s8;
typedef signed short s16;
typedef signed int s32;

/* function prototypes */
int wikilib_init (void);
int wikilib_run (void);
void invert_selection(int old_pos, int new_pos, int start_pos, int height);
unsigned long timer_get(void);
unsigned long time_diff(unsigned long t2, unsigned long t1);
enum {
	Tick_TicksPerMicroSecond = 60,
	Tick_TicksPerMilliSecond = Tick_TicksPerMicroSecond * 1000,
	Tick_TicksPerSecond = Tick_TicksPerMilliSecond * 1000,
};

/* Every call site passes a literal -- seconds_to_ticks(0.3),
   seconds_to_ticks(LINK_ACTIVATION_TIME_THRESHOLD) and so on; there is not
   one variable argument in the tree.  Out of line in wikilib.c, none of
   that could fold, so each call ran __mulsf3 followed by __fixsfsi: 137
   instructions to recompute a constant.  Those two were the only soft
   float the firmware ever reached, 34,264 times in one 700 ms band of
   typing, 2.2% of everything executed.

   Inline, the compiler does the same multiply in the same single
   precision at compile time, so the value is unchanged -- this is not a
   switch to integer arithmetic, which would round differently.  */

static inline unsigned long seconds_to_ticks(float sec)
{
	long clock_ticks;

	clock_ticks = sec * Tick_TicksPerSecond;

	return clock_ticks;
}

void repaint_search(void);
void fatal_error_print(const char *file, int line, const char *format, ...)  __attribute__ ((noreturn, format (printf, 3, 4)));
#define fatal_error(format...)				\
	fatal_error_print(__FILE__, __LINE__, format)

void handle_search_key(struct keyboard_key *key, unsigned long ev_time);
void wikilib_reset_highlighting();
void draw_logo_or_type_a_word(int clear_start_x, int clear_start_y, int clear_end_x, int clear_end_y);
void clear_logo_or_type_a_word(int clear_start_x, int clear_start_y, int clear_end_x, int clear_end_y);
#endif /* WIKILIB_H */
