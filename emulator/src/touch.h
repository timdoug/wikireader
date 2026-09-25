#ifndef TOUCH_H
#define TOUCH_H

#include <stdint.h>
#include <stdbool.h>

#include "mem.h"
#include "c33.h"
#include "itc.h"
#include "cmu.h"

/*
 * Host-side queue of bytes waiting to be handed to the driver.
 *
 * The hardware receive FIFO is only 4 bytes deep (RXDxNUM[1:0] in the status
 * register counts up to 4), but a CTP packet is 6, so on real silicon a
 * packet spans at least two receive interrupts. Queueing a whole packet
 * here and letting CTP_interrupt's "while (RDBFx)" loop drain it in one go
 * is the one deliberate divergence: the driver is a byte-at-a-time state
 * machine, so it reassembles the packet identically either way, and a
 * 4-byte queue would instead mean dropping bytes and setting OERx.
 */
#define TOUCH_FIFO  64
/*
 * The panel reports at exactly twice the LCD resolution, so a pixel
 * coordinate is sent shifted left by one:
 *
 *     // CTP is double the LCD resolution, so correct the x and y values
 *     e.touch.x = x >> 1;                (samo-lib/grifo/src/CTP.c:140)
 */
#define CTP_SHIFT   1

/*
 * What the panel transmits at: CTP_BPS, which is 9600 on this board.  The
 * library default in samo-lib/include/samo.h is 38400 and the board header
 * samo-lib/include/boards/samo_a1.h overrides it; the board header is the
 * one that counts.
 *
 * A receiver has to be told the same number, and one that is told a
 * different number hears nothing usable -- worth modelling, because it is
 * otherwise a silent failure that looks like a dead panel.
 */
#define CTP_BPS  9600u

/* How far the two rates may differ before the framing goes.  Real receivers
 * sample in the middle of the bit and lose the stop bit somewhere past three
 * per cent of accumulated error over ten bits. */
#define CTP_BAUD_TOLERANCE 3

struct touch {
	uint8_t  fifo[TOUCH_FIFO];
	unsigned head, tail;
	unsigned long events, bytes_read;
	const struct itc *itc;   /* for the configured interrupt priority */

	/* The receiver's side of the link, as the guest has configured it. */
	uint8_t  irda;           /* DIVMD lives here */
	uint16_t brt;            /* baud rate timer reload */
	uint8_t  errors;         /* FERx and friends, cleared by writing them */
	uint32_t clock_hz;       /* what the loader leaves the machine on */
	const struct cmu *cmu;   /* ...and what it is running on now */
	unsigned long garbled;   /* packets lost to a mismatched rate */
	unsigned long gated_accesses; /* register traffic with the EFSIO gates off */
	unsigned long gated_packets;  /* packets that arrived at an unclocked port */
};

void touch_attach(struct mem *m, struct touch *t, const struct itc *itc);
void touch_reset(struct touch *t);
/* The clock the baud rate generator divides down, for a machine that has
   not chosen one: an image loaded straight in starts where the loader would
   have left it. */
void touch_set_clock(struct touch *t, uint32_t hz);
/* Where to read the clock once the program has chosen one for itself. */
void touch_set_cmu(struct touch *t, const struct cmu *cmu);
/* The rate the guest has configured, or 0 if it has configured none. */
uint32_t touch_baud(const struct touch *t);
void touch_post(struct touch *t, struct c33 *cpu, int x, int y, bool pressed);
void touch_poll(struct touch *t, struct c33 *cpu);
/* Pixel centre of an on-screen keyboard key, or false if unmapped. */
bool touch_key_pos(char ch, int *x, int *y);

#endif /* TOUCH_H */
