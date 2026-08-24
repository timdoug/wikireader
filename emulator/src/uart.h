#ifndef UART_H
#define UART_H

#include <stdio.h>
#include "mem.h"

struct uart {
	FILE         *out;
	unsigned long tx_count;
	char          capture[4096];
	size_t        capture_len;
};

void uart_attach(struct mem *m, struct uart *u, FILE *out);

#endif /* UART_H */
