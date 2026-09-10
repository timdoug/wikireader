#ifndef UART_H
#define UART_H

#include <stdio.h>
#include "mem.h"
#include "itc.h"

struct uart {
	FILE         *out;
	struct itc   *itc;
	uint8_t       rx[4], rx_head, rx_count, control, errors;
	unsigned long tx_count;
	char          capture[4096];
	size_t        capture_len;
};

void uart_attach(struct mem *m, struct uart *u, FILE *out);

void uart_reset(struct uart *u);
bool uart_can_receive(const struct uart *u);
bool uart_receive(struct uart *u, uint8_t byte);
void uart_poll(struct uart *u);

#endif /* UART_H */
