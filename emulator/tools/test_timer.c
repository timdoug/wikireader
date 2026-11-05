/* 16-bit timer pause/read behaviour used by Tick_get(). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/mem.h"
#include "../src/timer.h"

#define CR0A       (REG_BASE + 0x780)
#define CR0B       (REG_BASE + 0x782)
#define TC0        (REG_BASE + 0x784)
#define CTL0       (REG_BASE + 0x786)
#define CR1A       (REG_BASE + 0x788)
#define TC1        (REG_BASE + 0x78c)
#define CTL1       (REG_BASE + 0x78e)
#define TC2        (REG_BASE + 0x794)
#define TC5        (REG_BASE + 0x7ac)
#define CR5A       (REG_BASE + 0x7a8)
#define CR5B       (REG_BASE + 0x7aa)
#define CTL5       (REG_BASE + 0x7ae)
#define CR2A       (REG_BASE + 0x790)
#define CR2B       (REG_BASE + 0x792)
#define CTL2       (REG_BASE + 0x796)
#define CNT_PAUSE  (REG_BASE + 0x7dc)
#define ADVMODE    (REG_BASE + 0x7de)
#define CLKCTL2    (REG_BASE + 0x7e4)
#define CLKCTL0    (REG_BASE + 0x7e0)
#define CLKCTL5    (REG_BASE + 0x7ea)
#define DA16_0     (REG_BASE + 0x7d0)
#define PAUSE0     (1u << 0)
#define PAUSE2     (1u << 2)
#define PAUSE5     (1u << 5)
#define PRESET     (1u << 1)
#define PRUN       (1u << 0)
#define SELCRB     (1u << 5)
#define INITOL     (1u << 8)
#define PTM         (1u << 2)
#define CKSL        (1u << 3)
#define OUTINV      (1u << 4)
#define P16TON      (1u << 3)

static int fails;

static void check(const char *what, uint32_t got, uint32_t want)
{
	printf("%-62s %s\n", what, got == want ? "ok" : "FAIL");
	if (got != want) {
		printf("    got %u, wanted %u\n", got, want);
		fails++;
	}
}

static uint32_t tick_get(struct mem *mem)
{
	mem_write(mem, CNT_PAUSE, 2, PAUSE5 | PAUSE0);
	uint32_t count = mem_read(mem, TC0, 2);
	count |= mem_read(mem, TC5, 2) << 16;
	mem_write(mem, CNT_PAUSE, 2, 0);
	return count;
}

static uint32_t random_state = 0x33e07;

static uint32_t next_random(void)
{
	random_state = random_state * 1664525u + 1013904223u;
	return random_state;
}

/* Polling at instruction granularity and jumping across a wait must give
 * the same counters, fractional clocks, buffered comparisons and deadlines.
 * Include zero-time polls, stopped clocks, pauses and the timer 0/5 cascade.
 */
static void test_poll_cadence(void)
{
	static const uint32_t clocks[] = {
		0, 2, 3 | (5u << 8), 5, 3 | (7u << 20) | (3u << 2)
	};
	static const uint32_t steps[] = { 0, 1, 2, 59, 60, 4096, 1000000 };
	static const uint16_t bounds[] = { 0, 1, 9, 0x7fff, 0xffff };
	unsigned comparisons = 0;

	for (unsigned trial = 0; trial < 2000; trial++) {
		uint64_t fine_clk = 0, bulk_clk = 0;
		struct cmu cmu = {0};
		struct itc fine_itc = {0}, bulk_itc = {0};
		struct timerblk fine = {0};
		cmu_reset(&cmu);
		cmu.reg[(0x1b0c - CMU_BASE) / 4] = (9u << 4) | 1;
		fine.reg[(0x7de - 0x780) / 2] = next_random() >> 31;
		for (unsigned ch = 0; ch < 6; ch++) {
			fine.reg[ch * 4 + 3] = (next_random() >> 16) & 0x7d;
			fine.reg[(0x7e0 - 0x780) / 2 + ch] =
				(next_random() >> 16) & 0xf;
			fine.count[ch] = bounds[next_random() % 5];
			for (unsigned which = 0; which < 2; which++) {
				/* A=0 shares the B reset edge; its flag semantics
				 * are not part of this polling-cadence regression.
				 */
				fine.compare[ch][which] = which ?
					bounds[next_random() % 5] :
					bounds[1 + next_random() % 4];
				fine.compare_buffer[ch][which] =
					(uint16_t)(next_random() >> 16) | !which;
			}
		}
		/* Regularly exercise the board's externally clocked timer 5. */
		if (trial % 2 == 0) {
			fine.reg[3] = PRUN | PTM | (trial & 2 ? OUTINV : 0);
			fine.reg[(0x7e0 - 0x780) / 2] = P16TON;
			fine.reg[5 * 4 + 3] = PRUN | CKSL;
		}
		struct mem fine_mem = {0}, bulk_mem = {0};
		struct timerblk seed = fine;
		timer_attach(&fine_mem, &fine, &fine_clk, &fine_itc, &cmu);
		fine = seed;
		struct timerblk bulk;
		timer_attach(&bulk_mem, &bulk, &bulk_clk, &bulk_itc, &cmu);
		bulk = seed;
		fine.cycles = &fine_clk;
		bulk.cycles = &bulk_clk;
		fine.cmu = bulk.cmu = &cmu;
		fine.itc = &fine_itc;
		bulk.itc = &bulk_itc;

		for (unsigned segment = 0; segment < 8; segment++) {
			/* Change clocks and pause state only at shared poll points. */
			cmu.reg[(0x1b08 - CMU_BASE) / 4] =
				clocks[next_random() % 5];
			cmu.reg[(0x1b04 - CMU_BASE) / 4] =
				(next_random() >> 16) << 13;
			fine.reg[(0x7dc - 0x780) / 2] =
				bulk.reg[(0x7dc - 0x780) / 2] =
				(next_random() >> 16) & 0x3f;
			/* Synchronize the configuration boundary before advancing. */
			mem_read(&fine_mem, TC0, 2);
			mem_read(&bulk_mem, TC0, 2);
			for (unsigned poll = 0; poll < 32; poll++) {
				fine_clk += steps[next_random() % 7];
				timer_poll(&fine, NULL); /* ITC enables are all zero. */
				/* Counter reads force eager synchronization as an oracle. */
				mem_read(&fine_mem, TC0, 2);
			}
			bulk_clk = fine_clk;
			timer_poll(&bulk, NULL);
			mem_read(&bulk_mem, TC0, 2);
			comparisons++;
			if (memcmp(fine.count, bulk.count, sizeof fine.count) ||
			    memcmp(fine.phase, bulk.phase, sizeof fine.phase) ||
			    memcmp(fine.compare, bulk.compare, sizeof fine.compare) ||
			    memcmp(fine.fires, bulk.fires, sizeof fine.fires) ||
			    memcmp(fine_itc.reg, bulk_itc.reg, sizeof fine_itc.reg) ||
			    fine.last_raw != bulk.last_raw ||
			    fine.deadline_valid != bulk.deadline_valid ||
			    (fine.deadline_valid &&
			     fine.next_deadline != bulk.next_deadline)) {
				printf("poll cadence FAIL: trial %u segment %u\n",
				       trial, segment);
				fails++;
				return;
			}
		}
	}
	printf("poll cadence: %u counter/phase/match/deadline comparisons ok\n",
	       comparisons);
}

/* Clock/gate writes between polls must not retroactively change the deferred
 * interval. Compare lazy polls with a counter read after every eager poll.
 * MMIO writes, buffered comparisons, the cascade, resets and pending flags
 * all remain visible at the same instruction boundaries.
 */
static void test_lazy_poll(void)
{
	struct mem eager_mem = {0}, lazy_mem = {0};
	struct cmu eager_cmu, lazy_cmu;
	struct timerblk eager, lazy;
	struct itc eager_itc = {0}, lazy_itc = {0};
	uint64_t clk = 0;
	cmu_attach(&eager_mem, &eager_cmu);
	cmu_attach(&lazy_mem, &lazy_cmu);
	timer_attach(&eager_mem, &eager, &clk, &eager_itc, &eager_cmu);
	timer_attach(&lazy_mem, &lazy, &clk, &lazy_itc, &lazy_cmu);
	mem_write(&eager_mem, REG_BASE + 0x1b24, 4, 0x96);
	mem_write(&lazy_mem, REG_BASE + 0x1b24, 4, 0x96);
	static const uint32_t sources[] = {
		0, 2, 3 | (5u << 8), 5, 3 | (7u << 20) | (3u << 2)
	};
	static const uint32_t steps[] = {0, 1, 2, 59, 60, 4096, 1000000};
	unsigned comparisons = 0;
	for (unsigned step = 0; step < 20000; step++) {
		clk += steps[next_random() % 7];
		uint32_t addr = 0, value = 0;
		switch (next_random() % 12) {
		case 0:
			addr = REG_BASE + 0x1b08;
			value = sources[next_random() % 5];
			break;
		case 1:
			addr = REG_BASE + 0x1b04;
			value = (next_random() >> 16) << 13;
			break;
		case 2:
			addr = REG_BASE + 0x1b0c;
			value = (next_random() >> 16) & 0xff;
			break;
		case 3:
			addr = CNT_PAUSE;
			value = next_random() >> 16;
			break;
		case 4:
			addr = ADVMODE;
			value = next_random() >> 31;
			break;
		case 5:
			addr = REG_BASE + 0x7e0 + 2 * (next_random() % 6);
			value = next_random() >> 16;
			break;
		case 6: case 7: case 8:
			addr = REG_BASE + 0x780 + 2 * (next_random() % 24);
			value = next_random() >> 16;
			break;
		case 9:
			addr = DA16_0 + 2 * (next_random() % 3);
			value = next_random() >> 16;
			break;
		case 10:
			if (step % 97 == 0) {
				cmu_reset(&eager_cmu);
				cmu_reset(&lazy_cmu);
				timer_reset(&eager);
				timer_reset(&lazy);
				mem_write(&eager_mem, REG_BASE + 0x1b24, 4, 0x96);
				mem_write(&lazy_mem, REG_BASE + 0x1b24, 4, 0x96);
			}
			break;
		}
		if (addr) {
			unsigned size = addr >= REG_BASE + CMU_BASE ? 4 : 2;
			mem_write(&eager_mem, addr, size, value);
			mem_write(&lazy_mem, addr, size, value);
		}
		timer_poll(&eager, NULL);
		mem_read(&eager_mem, TC0, 2);
		timer_poll(&lazy, NULL);
		comparisons++;
		if (memcmp(eager.fires, lazy.fires, sizeof eager.fires) ||
		    memcmp(eager_itc.reg, lazy_itc.reg, sizeof eager_itc.reg) ||
		    eager.deadline_valid != lazy.deadline_valid ||
		    (eager.deadline_valid && eager.next_deadline != lazy.next_deadline)) {
			printf("lazy event/clock boundary FAIL: step %u\n", step);
			fails++;
			return;
		}
		if (step % 8 == 0) {
			mem_read(&lazy_mem, TC0, 2);
			if (memcmp(eager.count, lazy.count, sizeof eager.count) ||
			    memcmp(eager.phase, lazy.phase, sizeof eager.phase) ||
			    memcmp(eager.compare, lazy.compare, sizeof eager.compare)) {
				printf("lazy MMIO synchronization FAIL: step %u\n", step);
				fails++;
				return;
			}
		}
	}
	printf("lazy polling: %u event/clock boundary comparisons ok\n", comparisons);
}

static void test_lazy_boundaries(void)
{
	struct mem mem = {0};
	struct cmu cmu = {0};
	struct timerblk timer;
	struct itc itc = {0};
	uint64_t clk = 0;
	cmu_attach(&mem, &cmu);
	timer_attach(&mem, &timer, &clk, &itc, &cmu);
	mem_write(&mem, REG_BASE + 0x1b24, 4, 0x96);
	mem_write(&mem, REG_BASE + 0x1b0c, 4, (9u << 4) | 1);
	mem_write(&mem, REG_BASE + 0x1b08, 4, 3 | (7u << 20) | (3u << 2));
	mem_write(&mem, CR0A, 2, 10);
	mem_write(&mem, CR0B, 2, 19);
	mem_write(&mem, CLKCTL0, 2, P16TON);
	mem_write(&mem, CTL0, 2, PRUN);
	clk = 5;
	timer_poll(&timer, NULL);
	clk = 6;
	mem_write(&mem, REG_BASE + 0x1b08, 4, 2); /* 60 MHz -> 48 MHz */
	timer_poll(&timer, NULL);
	check("clock change preserves the deferred old-rate interval",
	      mem_read(&mem, TC0, 2), 6);
	check("clock change does not retroactively apply the new rate",
	      timer.phase[0], 0);
	clk = 7;
	mem_write(&mem, REG_BASE + 0x1b04, 4, 0);
	timer_poll(&timer, NULL);
	clk = 100;
	timer_poll(&timer, NULL);
	check("a gated timer retains count without charging the stopped interval",
	      mem_read(&mem, TC0, 2), 6);
	clk = 101;
	mem_write(&mem, REG_BASE + 0x1b04, 4, 1u << 13);
	timer_poll(&timer, NULL);
	check("ungating retains the fractional clock from before the pause",
	      mem_read(&mem, TC0, 2), 6);
	check("ungating does not charge the stopped interval",
	      timer.phase[0], 48000000);
	mem_write(&mem, REG_BASE + 0x1b08, 4, 3 | (7u << 20) | (3u << 2));
	mem_write(&mem, CTL0, 2, PRESET | PRUN);
	clk += 9;
	timer_poll(&timer, NULL);
	check("comparison A stays quiet before its exact deadline", timer.fires[0][0], 0);
	clk++;
	timer_poll(&timer, NULL);
	check("comparison A fires on its exact deadline", timer.fires[0][0], 1);
	clk += 10;
	timer_poll(&timer, NULL);
	check("comparison B fires on the inclusive counter wrap", timer.fires[0][1], 1);
	check("counter reads synchronize after comparison B", mem_read(&mem, TC0, 2), 0);
	/* Disabled timer causes must not suppress unrelated pending IRQs. */
	itc.reg[0x70] = 1u << 4; /* key input 0 enable */
	itc.reg[0x62] = 7;       /* key input 0 priority */
	itc_set_flag(&itc, 20);
	struct c33 cpu = {0};
	timer_poll(&timer, &cpu);
	check("a deferred timer poll still presents unrelated pending IRQs",
	      cpu.irq_vector, 20);
}

static void test_clock_write_boundaries(void)
{
	struct mem mem = {0};
	struct cmu cmu;
	struct timerblk timer;
	uint64_t clk = 0;
	cmu_attach(&mem, &cmu);
	timer_attach(&mem, &timer, &clk, NULL, &cmu);
	mem_write(&mem, REG_BASE + 0x1b24, 4, 0x96);
	mem_write(&mem, REG_BASE + 0x1b0c, 4, (9u << 4) | 1);
	mem_write(&mem, REG_BASE + 0x1b08, 4, 3 | (7u << 20) | (3u << 2));
	mem_write(&mem, CR0A, 2, 40);
	mem_write(&mem, CR0B, 2, 0xffff);
	mem_write(&mem, CLKCTL0, 2, P16TON);
	mem_write(&mem, CTL0, 2, PRUN);
	clk = 40; /* no polls between setup and the clock write */
	mem_write(&mem, REG_BASE + 0x1b08, 4, 2);
	check("rate switch settles all preceding clocks at the old rate", timer.count[0], 40);
	check("rate switch preserves a comparison due at the write", timer.fires[0][0], 1);
	clk = 50;
	mem_write(&mem, REG_BASE + 0x1b04, 4, 0);
	check("gate close settles all preceding clocks at the old rate", timer.count[0], 48);
	clk = 1000;
	mem_write(&mem, REG_BASE + 0x1b04, 4, 1u << 13);
	check("gate open excludes the entire unpolled stopped interval", timer.count[0], 48);
	clk = 1005;
	check("count resumes at the newly selected rate", mem_read(&mem, TC0, 2), 52);
	cmu_reset(&cmu);
	timer_reset(&timer);
	mem_write(&mem, REG_BASE + 0x1b24, 4, 0x96);
	mem_write(&mem, CR0B, 2, 0xffff);
	mem_write(&mem, CLKCTL0, 2, P16TON);
	mem_write(&mem, CTL0, 2, PRUN);
	clk += 10;
	mem_write(&mem, REG_BASE + 0x1b04, 4, 0);
	check("timer clock listener survives a whole-machine reset", timer.count[0], 8);
}

int main(void)
{
	struct mem mem;
	struct timerblk timer;
	uint64_t clk = 100;

	if (!mem_init(&mem))
		return 1;
	timer_attach(&mem, &timer, &clk, NULL, NULL);

	mem_write(&mem, CNT_PAUSE, 2, PAUSE5 | PAUSE0);
	check("count pause is read-only in standard mode",
	      mem_read(&mem, CNT_PAUSE, 2), 0);
	mem_write(&mem, ADVMODE, 2, 0xffff);
	check("only T16ADV survives in the mode register",
	      mem_read(&mem, ADVMODE, 2), 1);

	/* Advanced-only fields must remain inert until advanced mode is set. */
	mem_write(&mem, ADVMODE, 2, 0);
	mem_write(&mem, TC1, 2, 0x1234);
	check("counter writes are ignored in standard mode",
	      mem_read(&mem, TC1, 2), 0);
	mem_write(&mem, CTL1, 2, INITOL);
	check("INITOL writes are ignored in standard mode",
	      mem_read(&mem, CTL1, 2), 0);
	mem_write(&mem, DA16_0, 2, 0xabcd);
	check("DA16 writes are ignored in standard mode",
	      mem_read(&mem, DA16_0, 2), 0);
	mem_write(&mem, ADVMODE, 2, 1);
	mem_write(&mem, TC1, 2, 0x1234);
	check("counter writes are accepted in advanced mode",
	      mem_read(&mem, TC1, 2), 0x1234);
	mem_write(&mem, CTL1, 2, INITOL);
	check("INITOL writes are accepted in advanced mode",
	      mem_read(&mem, CTL1, 2), INITOL);

	/* SELCRB exposes a separate bank and PRESET loads it atomically. */
	mem_write(&mem, CR1A, 2, 0x1111);
	mem_write(&mem, CTL1, 2, SELCRB);
	check("comparison buffer starts independently from active data",
	      mem_read(&mem, CR1A, 2), 0);
	mem_write(&mem, CR1A, 2, 0x2222);
	mem_write(&mem, CTL1, 2, SELCRB | PRESET);
	check("PRESET remains a write-only command",
	      mem_read(&mem, CTL1, 2), SELCRB);
	mem_write(&mem, CTL1, 2, 0);
	check("PRESET loads the comparison buffer into the active register",
	      mem_read(&mem, CR1A, 2), 0x2222);
	check("PRESET resets the selected timer counter",
	      mem_read(&mem, TC1, 2), 0);

	/* DA16 follows each destination timer's active/buffer selection. */
	mem_write(&mem, CTL1, 2, SELCRB);
	mem_write(&mem, CTL2, 2, 0);
	mem_write(&mem, DA16_0, 2, 0xabcd);
	check("DA16 stores its complete source value", mem_read(&mem, DA16_0, 2),
	      0xabcd);
	check("DA16 writes its high ten bits to timer 1's selected CR A",
	      mem_read(&mem, CR1A, 2), 0x02af);
	check("DA16 writes its low six bits to timer 2's selected CR A",
	      mem_read(&mem, CR2A, 2), 0x000d);

	/* The board routes timer 0's inverted B edge into timer 5's input. */
	mem_write(&mem, CR0A, 2, 0x7fff);
	mem_write(&mem, CR0B, 2, 0xffff);
	mem_write(&mem, CLKCTL0, 2, P16TON);
	mem_write(&mem, CTL0, 2, OUTINV | PTM | PRESET);
	mem_write(&mem, CR5A, 2, 0x7fff);
	mem_write(&mem, CR5B, 2, 0xffff);
	mem_write(&mem, CLKCTL5, 2, P16TON);
	mem_write(&mem, CTL5, 2, CKSL | PRESET);
	mem_write(&mem, CNT_PAUSE, 2, PAUSE5 | PAUSE0);
	mem_write(&mem, CTL0, 2, OUTINV | PTM | PRUN);
	mem_write(&mem, CTL5, 2, CKSL | PRUN);
	mem_write(&mem, CNT_PAUSE, 2, 0);
	clk = 200;
	check("running timer 0 reflects elapsed MCLK clocks", tick_get(&mem), 100);
	check("Tick_get releases both paused channels",
	      mem_read(&mem, CNT_PAUSE, 2), 0);

	mem_write(&mem, CNT_PAUSE, 2, PAUSE5 | PAUSE0);
	check("count-pause register reads back per-channel bits",
	      mem_read(&mem, CNT_PAUSE, 2), PAUSE5 | PAUSE0);
	clk = 1000;
	check("paused timer retains its captured count", mem_read(&mem, TC0, 2),
	      100);
	mem_write(&mem, CNT_PAUSE, 2, 0);
	clk = 1050;
	check("timer resumes without charging the paused interval",
	      tick_get(&mem), 150);

	/* Pausing an unrelated channel must not freeze the 0/5 tick pair. */
	mem_write(&mem, CNT_PAUSE, 2, PAUSE2);
	clk = 1100;
	check("an unrelated channel pause does not stop timer 0/5",
	      mem_read(&mem, TC0, 2), 200);
	clk += 65536;
	check("timer 0's rising B edge clocks external timer 5",
	      tick_get(&mem), 0x000100c8);

	/*
	 * The firmware's own cascade (drivers/src/tick.c) leaves OUTINV clear
	 * and puts comparison A at zero, so the match that clocks timer 5 is
	 * the one the counter reset presents to comparator A.
	 */
	mem_write(&mem, CR0A, 2, 0);
	mem_write(&mem, CR0B, 2, 0xffff);
	mem_write(&mem, CTL0, 2, PTM | PRESET);
	mem_write(&mem, CTL5, 2, CKSL | PRESET);
	mem_write(&mem, CNT_PAUSE, 2, PAUSE5 | PAUSE0);
	mem_write(&mem, CTL0, 2, PTM | PRUN);
	mem_write(&mem, CTL5, 2, CKSL | PRUN);
	mem_write(&mem, CNT_PAUSE, 2, 0);
	clk += 65536 + 100;
	check("a comparison A of zero clocks timer 5 at each wrap",
	      tick_get(&mem), 0x00010064);

	mem_write(&mem, CTL2, 2, PRESET);
	check("the PRESET command always reads back zero",
	      mem_read(&mem, CTL2, 2), 0);
	mem_write(&mem, CLKCTL2, 2, 0xffff);
	check("reserved clock-control bits read zero",
	      mem_read(&mem, CLKCTL2, 2), 0x000f);
	mem_write(&mem, CR2A, 2, 5);
	mem_write(&mem, CR2B, 2, 9);
	mem_write(&mem, CTL0, 2, 0);
	mem_write(&mem, CTL5, 2, 0);
	mem_write(&mem, CTL2, 2, PRESET | PRUN);
	clk += 10u * 4096u;
	check("timer 2 resets after counting through comparison B",
	      mem_read(&mem, TC2, 2), 0);
	check("timer 2 records its comparison B match", timer.fires[2][1], 1);

	/* OSC3 is 48 MHz; OSC3/32 is 1.5 MHz against the raw 60 MHz clock. */
	struct cmu cmu = {0};
	cmu_reset(&cmu);
	cmu.reg[(0x1b08 - CMU_BASE) / 4] = (5u << 8) | (1u << 1);
	timer.cmu = &cmu;
	mem_write(&mem, CR2A, 2, 0);
	mem_write(&mem, CR2B, 2, 9);
	mem_write(&mem, CTL2, 2, PRESET | PRUN);
	check("OSC3/32 scales the timer against the 60 MHz raw timebase",
	      (uint32_t)(timer.next_deadline - clk), 10u * 4096u * 40u);

	setenv("WREMU_SUSPEND_DIV", "60", 1);
	mem_write(&mem, CTL2, 2, PRESET | PRUN);
	check("debug speedup applies to the deep-suspend timer",
	      (uint32_t)(timer.next_deadline - clk),
	      (10u * 4096u * 40u + 59u) / 60u);
	/* Without a CMU attached the input is the full 60 MHz MCLK. */
	timer.cmu = NULL;
	mem_write(&mem, CLKCTL2, 2, P16TON | 6u); /* /1024 */
	mem_write(&mem, CTL2, 2, PRESET | PRUN);
	check("debug speedup leaves short CPU-only waits unchanged",
	      (uint32_t)(timer.next_deadline - clk), 10u * 1024u);
	clk += 10u * 1024u;
	check("short wait counter also ignores the debug speedup",
	      mem_read(&mem, TC2, 2), 0);
	unsetenv("WREMU_SUSPEND_DIV");
	test_poll_cadence();
	test_lazy_poll();
	test_lazy_boundaries();
	test_clock_write_boundaries();

	mem_free(&mem);
	printf("\n%s\n", fails ? "FAILURES" : "all timer tests passed");
	return fails != 0;
}
