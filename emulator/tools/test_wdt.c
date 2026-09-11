/*
 * Watchdog timer.
 *
 * grifo arms it for twenty seconds and kicks it constantly, so the thing
 * that matters is not the timeout firing but the timeout *not* firing when
 * it should not. The trap is the clock gate: the suspend code leaves
 * WDT_CKE out of the set of clocks it enables, so the counter stops for the
 * whole two-minute suspend. Modelling the counter as "now minus the last
 * kick" looks right and is wrong -- it banks the gated time and fires the
 * instant the clock comes back, turning every suspend into a reset.
 */
#include <stdio.h>
#include <string.h>

#include "../src/mem.h"
#include "../src/cmu.h"
#include "../src/wdt.h"

#define WD_WP    (REG_BASE + 0x660)
#define WD_EN    (REG_BASE + 0x662)
#define WD_COMP  (REG_BASE + 0x664)
#define WD_CNT   (REG_BASE + 0x668)
#define WD_CNTL  (REG_BASE + 0x66c)

#define CMU_PROTECT   (REG_BASE + 0x1b24)
#define CMU_GATEDCLK1 (REG_BASE + 0x1b04)

#define WP_OFF   0x96
#define RUNSTP   (1u << 4)
#define NMIEN    (1u << 1)
#define RESEN    (1u << 0)
#define WDRESEN  (1u << 0)
#define WDT_CKE  (1u << 9)

static int fails;

static void ok(const char *what, bool cond)
{
	printf("%-62s %s\n", what, cond ? "ok" : "FAILED");
	if (!cond)
		fails++;
}

int main(void)
{
	struct mem mem;
	struct cmu cmu;
	struct wdt w;
	uint64_t clk = 0;

	if (!mem_init(&mem))
		return 1;
	cmu_attach(&mem, &cmu);
	wdt_attach(&mem, &w, &cmu, &clk);

	/* The clock has to be running before the counter can count. */
	mem_write(&mem, CMU_PROTECT, 4, 0x96);
	mem_write(&mem, CMU_GATEDCLK1, 4, WDT_CKE);

	/* Writes to the timeout are protected; the kick is not. */
	mem_write(&mem, WD_COMP, 4, 1234);
	ok("a protected write to COMP is rejected",
	   mem_read(&mem, WD_COMP, 4) == 0 && w.blocked == 1);

	mem_write(&mem, WD_WP, 2, WP_OFF);
	mem_write(&mem, WD_COMP, 4, 0xc00003e8);
	mem_write(&mem, WD_EN, 2, RUNSTP | NMIEN | RESEN);
	mem_write(&mem, WD_WP, 2, 0x00);
	ok("COMP implements its documented 30-bit width",
	   mem_read(&mem, WD_COMP, 4) == 1000);
	ok("and the register is protected again", ({
		mem_write(&mem, WD_COMP, 4, 7);
		mem_read(&mem, WD_COMP, 4) == 1000;
	}));

	/* The counter follows the clock. */
	clk = 400;
	wdt_poll(&w);
	ok("the counter follows the clock", mem_read(&mem, WD_CNT, 4) == 400);
	ok("and has not timed out yet", !w.expired);

	/* A kick clears it, which is all Watchdog_KeepAlive does. */
	mem_write(&mem, WD_CNTL, 2, WDRESEN);
	ok("a kick clears the counter", mem_read(&mem, WD_CNT, 4) == 0);
	ok("and is counted", w.kicks == 1);

	/*
	 * The gate. Time passing with the clock off must not be banked: this
	 * is a whole suspend's worth of cycles, far past the timeout.
	 */
	mem_write(&mem, CMU_PROTECT, 4, 0x96);
	mem_write(&mem, CMU_GATEDCLK1, 4, 0);        /* WDT_CKE off */
	ok("a gated clock stops the watchdog", !wdt_running(&w));

	clk += 50 * 1000;                            /* far beyond COMP */
	wdt_poll(&w);
	ok("time passing while gated does not time it out", !w.expired);
	ok("and does not accumulate", mem_read(&mem, WD_CNT, 4) == 0);

	mem_write(&mem, CMU_GATEDCLK1, 4, WDT_CKE);  /* resume */
	wdt_poll(&w);
	ok("re-enabling the clock does not bank the gated time", !w.expired);

	/* Left alone with the clock on, it does fire. */
	clk += 999;
	wdt_poll(&w);
	ok("just under the timeout is still quiet", !w.expired);
	clk += 1;
	wdt_poll(&w);
	ok("reaching CMPDT is still within the cycle", !w.expired);
	clk += 1;
	wdt_poll(&w);
	ok("the CMPDT + 1 clock times out", w.expired);
	ok("both enabled outputs also assert NMI", w.nmi_pending);
	ok("a comparison match resets the up-counter", wdt_count(&w) == 0);
	ok("and is counted once", w.timeouts == 1);

	/* Output selection is independent of counting; reset has priority in
	 * the machine loop only when both signals are asserted. */
	wdt_reset(&w);
	mem_write(&mem, WD_WP, 2, WP_OFF);
	mem_write(&mem, WD_COMP, 4, 31);
	mem_write(&mem, WD_EN, 2, RUNSTP | NMIEN);
	mem_write(&mem, WD_CNTL, 2, WDRESEN);
	clk += 32;
	wdt_poll(&w);
	ok("NMI-only mode asserts NMI", w.nmi_pending);
	ok("NMI-only mode does not request reset", !w.expired);

	wdt_reset(&w);
	mem_write(&mem, WD_WP, 2, WP_OFF);
	mem_write(&mem, WD_COMP, 4, 31);
	mem_write(&mem, WD_EN, 2, RUNSTP | RESEN);
	mem_write(&mem, WD_CNTL, 2, WDRESEN);
	clk += 32;
	wdt_poll(&w);
	ok("reset-only mode requests reset", w.expired);
	ok("reset-only mode does not assert NMI", !w.nmi_pending);

	wdt_reset(&w);
	mem_write(&mem, WD_WP, 2, WP_OFF);
	mem_write(&mem, WD_COMP, 4, 31);
	mem_write(&mem, WD_EN, 2, RUNSTP);
	mem_write(&mem, WD_CNTL, 2, WDRESEN);
	clk += 32;
	wdt_poll(&w);
	ok("disabled outputs request neither reset nor NMI",
	   !w.expired && !w.nmi_pending);
	ok("a match still resets the counter with outputs disabled",
	   wdt_count(&w) == 0);

	/* A stopped watchdog is a stopped watchdog. */
	wdt_reset(&w);
	mem_write(&mem, WD_WP, 2, WP_OFF);
	mem_write(&mem, WD_COMP, 4, 100);
	mem_write(&mem, WD_EN, 2, 0);                /* RUNSTP clear */
	clk += 10000;
	wdt_poll(&w);
	ok("with RUNSTP clear it never fires", !w.expired);

	/*
	 * The timeout as a deadline. An idle guest skips straight to the
	 * nearest one, so a watchdog that does not offer its own gets skipped
	 * over: System_reboot() halts with nothing else running, and the
	 * reset it is waiting for never arrives.
	 */
	{
		uint64_t delay = 12345;

		ok("a stopped watchdog offers no deadline",
		   !wdt_deadline(&w, &delay));

		wdt_reset(&w);
		mem_write(&mem, WD_WP, 2, WP_OFF);
		mem_write(&mem, WD_COMP, 4, 99);
		mem_write(&mem, WD_EN, 2, RUNSTP | RESEN);
		mem_write(&mem, WD_CNTL, 2, WDRESEN);
		wdt_poll(&w);
		ok("a fresh counter is due in CMPDT + 1 clocks",
		   wdt_deadline(&w, &delay) && delay == 100);

		clk += 60;
		wdt_poll(&w);
		ok("and the deadline closes as it counts",
		   wdt_deadline(&w, &delay) && delay == 40);

		mem_write(&mem, WD_CNTL, 2, WDRESEN);
		ok("a kick pushes it back out to the full period",
		   wdt_deadline(&w, &delay) && delay == 100);

		/* Skipping exactly that far has to be enough to fire it,
		 * or the caller wakes to find nothing to do and skips again. */
		clk += delay;
		wdt_poll(&w);
		ok("skipping the deadline times it out", w.expired);
		ok("and an expired watchdog offers no further deadline",
		   !wdt_deadline(&w, &delay));

		/* Gating the clock stops the counter, so there is no longer a
		 * time at which it will fire. */
		wdt_reset(&w);
		mem_write(&mem, WD_WP, 2, WP_OFF);
		mem_write(&mem, WD_COMP, 4, 99);
		mem_write(&mem, WD_EN, 2, RUNSTP | RESEN);
		mem_write(&mem, WD_CNTL, 2, WDRESEN);
		mem_write(&mem, CMU_GATEDCLK1, 4, 0);
		ok("a gated watchdog offers no deadline",
		   !wdt_deadline(&w, &delay));
		mem_write(&mem, CMU_GATEDCLK1, 4, WDT_CKE);
		ok("and offers one again when the clock comes back",
		   wdt_deadline(&w, &delay) && delay == 100);
	}

	mem_free(&mem);
	printf("\n%s\n", fails ? "FAILURES" : "all watchdog tests passed");
	return fails != 0;
}
