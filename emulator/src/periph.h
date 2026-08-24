#ifndef PERIPH_H
#define PERIPH_H

#include "mem.h"

struct periph {
	unsigned long adc_writes;
};

void periph_attach(struct mem *m, struct periph *p);

#endif /* PERIPH_H */
