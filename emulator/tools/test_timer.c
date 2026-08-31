/* 16-bit timer pause/read behaviour used by Tick_get(). */
#include <stdio.h>

#include "../src/mem.h"
#include "../src/timer.h"

#define TC0        (REG_BASE + 0x784)
#define CR1A       (REG_BASE + 0x788)
#define TC1        (REG_BASE + 0x78c)
#define CTL1       (REG_BASE + 0x78e)
#define TC5        (REG_BASE + 0x7ac)
#define CR2A       (REG_BASE + 0x790)
#define CR2B       (REG_BASE + 0x792)
#define CTL2       (REG_BASE + 0x796)
#define CNT_PAUSE  (REG_BASE + 0x7dc)
#define ADVMODE    (REG_BASE + 0x7de)
#define CLKCTL2    (REG_BASE + 0x7e4)
#define DA16_0     (REG_BASE + 0x7d0)
#define PAUSE0     (1u << 0)
#define PAUSE2     (1u << 2)
#define PAUSE5     (1u << 5)
#define PRESET     (1u << 1)
#define PRUN       (1u << 0)
#define SELCRB     (1u << 5)
#define INITOL     (1u << 8)

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

	mem_write(&mem, CTL2, 2, PRESET);
	check("the PRESET command always reads back zero",
	      mem_read(&mem, CTL2, 2), 0);
	mem_write(&mem, CLKCTL2, 2, 0xffff);
	check("reserved clock-control bits read zero",
	      mem_read(&mem, CLKCTL2, 2), 0x000f);
	mem_write(&mem, CR2A, 2, 5);
	mem_write(&mem, CR2B, 2, 9);
	mem_write(&mem, CTL2, 2, PRUN);
	check("timer 2 uses comparison B and counts it inclusively",
	      (uint32_t)(timer.t2_deadline - clk), 10u * 4096u * 32u);

	mem_free(&mem);
	printf("\n%s\n", fails ? "FAILURES" : "all timer tests passed");
	return fails != 0;
}
