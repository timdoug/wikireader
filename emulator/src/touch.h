#ifndef TOUCH_H
#define TOUCH_H

#include <stdint.h>
#include <stdbool.h>

#include "mem.h"
#include "c33.h"
#include "itc.h"

#define TOUCH_FIFO  64
/*
 * The panel reports at exactly twice the LCD resolution, so a pixel
 * coordinate is sent shifted left by one:
 *
 *     // CTP is double the LCD resolution, so correct the x and y values
 *     e.touch.x = x >> 1;                (samo-lib/grifo/src/CTP.c:140)
 */
#define CTP_SHIFT   1

struct touch {
	uint8_t  fifo[TOUCH_FIFO];
	unsigned head, tail;
	unsigned long events, bytes_read;
	const struct itc *itc;   /* for the configured interrupt priority */
};

void touch_attach(struct mem *m, struct touch *t, const struct itc *itc);
void touch_post(struct touch *t, struct c33 *cpu, int x, int y, bool pressed);
void touch_poll(struct touch *t, struct c33 *cpu);
/* Pixel centre of an on-screen keyboard key, or false if unmapped. */
bool touch_key_pos(char ch, int *x, int *y);

#endif /* TOUCH_H */
