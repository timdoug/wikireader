#ifndef WDT_H
#define WDT_H

#include <stdint.h>
#include <stdbool.h>

#include "mem.h"
#include "cmu.h"

/* Watchdog timer, REG_BASE+0x660..0x66f. */
#define WDT_BASE 0x0660u
#define WDT_LEN  0x0010u

struct wdt {
	uint32_t   comp;         /* timeout, in counter ticks */
	uint32_t   en;           /* REG_WD_EN: RUNSTP, NMIEN, RESEN */
	bool       unlocked;     /* REG_WD_WP holds 0x96 */
	/*
	 * Accumulated ticks, not a timestamp difference. The clock is gated,
	 * so time that passes while it is off must not count -- a difference
	 * against the last kick would quietly bank a whole suspend and fire
	 * the moment the clock came back.
	 */
	uint64_t   count;
	uint64_t   last_clk;     /* clk at the previous poll */
	const struct cmu *cmu;   /* for WDT_CKE: a gated clock does not count */
	const uint64_t *clk;

	bool          expired;   /* reset output is asserted */
	bool          nmi_pending; /* NMI output pulse awaits CPU delivery */
	unsigned long kicks, timeouts;
	unsigned long blocked;   /* writes rejected by the protect register */
};

void wdt_attach(struct mem *m, struct wdt *w, const struct cmu *cmu,
		const uint64_t *clk);
void wdt_reset(struct wdt *w);
/* True once the counter has run past REG_WD_COMP without being kicked. */
void wdt_poll(struct wdt *w);
/* Ticks since the last kick, which is what REG_WD_CNT reads back. */
uint32_t wdt_count(const struct wdt *w);
bool wdt_running(const struct wdt *w);

#endif /* WDT_H */
