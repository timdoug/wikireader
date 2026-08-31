/* 16-bit timer pause/read behaviour used by Tick_get(). */
#include <stdio.h>

#include "../src/mem.h"
#include "../src/timer.h"

#define TC0        (REG_BASE + 0x784)
#define TC5        (REG_BASE + 0x7ac)
#define CNT_PAUSE  (REG_BASE + 0x7dc)
#define ADVMODE    (REG_BASE + 0x7de)
#define PAUSE0     (1u << 0)
#define PAUSE2     (1u << 2)
#define PAUSE5     (1u << 5)

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

int main(void)
{
	struct mem mem;
	struct timerblk timer;
	uint64_t clk = 100;

	if (!mem_init(&mem))
		return 1;
	timer_attach(&mem, &timer, &clk, NULL);
	mem_write(&mem, ADVMODE, 2, 1); /* CNT_PAUSE is enabled in ADV mode. */

	check("running timer reflects the MCLK count", tick_get(&mem), 100);
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

	mem_free(&mem);
	printf("\n%s\n", fails ? "FAILURES" : "all timer tests passed");
	return fails != 0;
}
