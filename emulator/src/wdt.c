/*
 * Watchdog timer.
 *
 * grifo arms it for twenty seconds and then kicks it constantly -- from the
 * main loop, from the suspend path and around every card access -- which is
 * why it was the busiest thing in the register map long after every other
 * peripheral had been modelled: 118,882 writes in one session, all of them
 * landing in an unclaimed hole.
 *
 * Watchdog_SetTimeout (samo-lib/grifo/src/watchdog.c) is the whole contract:
 *
 *	REG_WD_WP = WD_WP_OFF;          // 0x96 lifts the write protection
 *	REG_WD_COMP = WatchdogTimeout;  // MCLK * 20
 *	REG_WD_CNTL = WDRESEN;          // clear the counter
 *	REG_WD_EN = RUNSTP | NMIEN | RESEN;
 *	REG_WD_WP = WD_WP_ON;           // and lock it again
 *
 * The counter is not free-running: WDT_CKE in the CMU gates its clock, and
 * the suspend code deliberately leaves that bit out of the set it restores.
 * A watchdog that kept counting through a two-minute suspend would fire long
 * before the device was due to wake, so the gate is not a detail.
 */
#include <string.h>

#include "wdt.h"

#define OFF_WP    0x00
#define OFF_EN    0x02
#define OFF_COMP  0x04
#define OFF_CNT   0x08
#define OFF_CNTL  0x0c

#define WP_OFF    0x96          /* protection lifted */

#define RUNSTP    (1u << 4)
#define NMIEN     (1u << 1)
#define RESEN     (1u << 0)

#define WDRESEN   (1u << 0)     /* written to CNTL to clear the counter */

#define WDT_CKE   (1u << 9)     /* in REG_CMU_GATEDCLK1 */
#define OFF_GATEDCLK1 ((0x1b04u - CMU_BASE) / 4)

bool wdt_running(const struct wdt *w)
{
	if (!(w->en & RUNSTP))
		return false;
	/* No clock, no count. This is what holds it off during a suspend. */
	if (w->cmu && !(w->cmu->reg[OFF_GATEDCLK1] & WDT_CKE))
		return false;
	return true;
}

uint32_t wdt_count(const struct wdt *w)
{
	return (uint32_t)w->count;
}

/* Bring the counter up to date, counting only the time the clock was on. */
static void accrue(struct wdt *w)
{
	uint64_t now = w->clk ? *w->clk : 0;

	if (now > w->last_clk && wdt_running(w))
		w->count += now - w->last_clk;
	w->last_clk = now;
}

void wdt_poll(struct wdt *w)
{
	accrue(w);
	if (w->expired || !w->comp || !wdt_running(w))
		return;
	if (w->count < w->comp)
		return;
	/*
	 * RESEN is set, and grifo's comment says reset takes priority over the
	 * NMI, so the run loop treats this as a reset rather than a vector.
	 */
	w->timeouts++;
	w->expired = true;
}

static bool wdt_mmio(void *ctx, uint32_t off, unsigned size, uint32_t *val,
		     bool is_write)
{
	struct wdt *w = ctx;
	uint32_t reg = off - WDT_BASE;

	if (is_write) {
		switch (reg) {
		case OFF_WP:
			w->unlocked = (*val & 0xff) == WP_OFF;
			return true;
		case OFF_CNTL:
			/*
			 * The kick. Not protected: it is the one thing the
			 * firmware does from ordinary code, thousands of times
			 * a second, without unlocking anything first.
			 */
			if (*val & WDRESEN) {
				accrue(w);
				w->count = 0;
				w->kicks++;
			}
			return true;
		case OFF_COMP:
		case OFF_EN:
			if (!w->unlocked) {
				w->blocked++;
				return true;
			}
			if (reg == OFF_COMP)
				w->comp = *val;
			else
				w->en = *val;
			return true;
		default:
			return true;      /* CNT is read-only; ignore stores */
		}
	}

	switch (reg) {
	case OFF_WP:   *val = w->unlocked ? WP_OFF : 0; break;
	case OFF_EN:   *val = w->en;   break;
	case OFF_COMP: *val = w->comp; break;
	case OFF_CNT:  *val = wdt_count(w); break;
	default:       *val = 0; break;
	}
	return true;
}

void wdt_reset(struct wdt *w)
{
	const struct cmu *cmu = w->cmu;
	const uint64_t *clk = w->clk;

	memset(w, 0, sizeof *w);
	w->cmu = cmu;
	w->clk = clk;
}

void wdt_attach(struct mem *m, struct wdt *w, const struct cmu *cmu,
		const uint64_t *clk)
{
	memset(w, 0, sizeof *w);
	w->cmu = cmu;
	w->clk = clk;
	mem_add_mmio(m, "wdt", WDT_BASE, WDT_LEN, wdt_mmio, w);
}
