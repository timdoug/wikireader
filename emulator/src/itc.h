#ifndef ITC_H
#define ITC_H

#include <stdint.h>
#include <stdbool.h>

#include "mem.h"

/* Interrupt controller register block, REG_BASE+0x200..0x2ff. */
#define ITC_BASE 0x0200u
#define ITC_LEN  0x0100u

struct itc {
	uint8_t reg[ITC_LEN];
	unsigned long writes;
};

void itc_attach(struct mem *m, struct itc *t);
/* Configured priority (0-7) for an interrupt vector, or 0 if unknown. */
unsigned itc_priority(const struct itc *t, unsigned vector);

#endif /* ITC_H */
