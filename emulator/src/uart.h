#ifndef UART_H
#define UART_H

#include <stdio.h>
#include "mem.h"
#include "itc.h"

struct uart {
	FILE         *out;
	struct itc   *itc;
	uint8_t       rx[4], rx_head, rx_count, control, errors;
	uint8_t       irda, brtrdl, brtrdm;   /* the baud rate, as programmed */
	const uint64_t *clock;                /* the CPU's cycle count, if timed */
	uint64_t      tx_done;                /* when the shift register frees */
	bool          tx_buffered;            /* a byte waiting behind it */
	unsigned long tx_count;
	char          capture[4096];
	size_t        capture_len;
	char          trace_line[256];
	size_t        trace_line_len;
};

void uart_attach(struct mem *m, struct uart *u, FILE *out);
/* Time transmission by this clock; without one a write completes at once. */
void uart_set_clock(struct uart *u, const uint64_t *clock);

void uart_reset(struct uart *u);
bool uart_can_receive(const struct uart *u);
bool uart_receive(struct uart *u, uint8_t byte);
void uart_poll(struct uart *u);

#endif /* UART_H */
