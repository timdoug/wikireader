#ifndef CMU_H
#define CMU_H

#include <stdint.h>
#include <stdbool.h>

#include "mem.h"

/* Clock management unit, REG_BASE+0x1b00..0x1b2f. */
#define CMU_BASE 0x1b00u
#define CMU_LEN  0x0030u

/*
 * OSC3 is a 48 MHz crystal on this board: grifo's comment on the PLL setup
 * reads "set up PLL for 48 MHz / 8 input -> 60 MHz output".
 */
#define OSC3_HZ  48000000u
#define OSC1_HZ  32768u

struct cmu {
	uint32_t reg[CMU_LEN / 4];
	unsigned long writes;
	unsigned long blocked;      /* writes rejected by the protect register */
};

void cmu_attach(struct mem *m, struct cmu *c);
/* Restore documented power-on values without re-registering the device. */
void cmu_reset(struct cmu *c);
/* System clock in Hz implied by the current register contents. */
uint32_t cmu_mclk_hz(const struct cmu *c);
/* True when WAKEUPWT selects automatic cancellation of SLEEP. */
bool cmu_slp_auto_wake(const struct cmu *c);
/* Whether MCLK is supplied to one of the six 16-bit timer channels. */
bool cmu_t16_enabled(const struct cmu *c, unsigned channel);
/* Whether MCLK is supplied to the shared IDMA/HSDMA controller. */
bool cmu_dma_enabled(const struct cmu *c);

#endif /* CMU_H */
