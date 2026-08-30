/*
 * Clock management unit test.
 *
 * Replays grifo's CMU_initialise() register values and checks the emulator
 * derives the frequency grifo's own comment claims: "set up PLL for 48 MHz
 * / 8 input -> 60 MHz output". That number is the timebase the tick timer
 * and Tick_TicksPerMicroSecond both assume, so deriving it from the
 * registers rather than assuming it is the point of the exercise.
 */

#include <stdio.h>

#include "../src/cmu.h"
#include "../src/mem.h"

/* samo-lib/include/regs.h, transcribed. */
#define CMU_PROTECT_OFF 0x96
#define CMU_PROTECT_ON  0x00

#define CMU_CLK_SEL_OSC3_DIV_32 (10 << 24)
#define PLLINDIV_8    (7 << 20)
#define LCDCDIV_12    (11 << 16)
#define OSC3DIV_32    (5 << 8)
#define OSCSEL_PLL    (3 << 2)
#define OSCSEL_OSC1   (1 << 2)
#define SOSC3         (1 << 1)

#define PLLCS         (0 << 22)
#define PLLBYP        (0 << 21)
#define PLLCP         (0x10 << 16)
#define PLLVC_100MHz_120MHz (1 << 12)
#define PLLRS_5MHz_20MHz    (10 << 8)
#define PLLN_X10      (9 << 4)
#define PLLV_DIV_2    (1 << 2)
#define PLLPOWR       (1 << 0)

/* grifo's final CLKCNTL and PLL values. */
#define GRIFO_CLKCNTL (CMU_CLK_SEL_OSC3_DIV_32 | PLLINDIV_8 | LCDCDIV_12 | \
		       OSC3DIV_32 | OSCSEL_PLL | SOSC3)
#define GRIFO_PLL     (PLLCS | PLLBYP | PLLCP | PLLVC_100MHz_120MHz | \
		       PLLRS_5MHz_20MHz | PLLN_X10 | PLLV_DIV_2 | PLLPOWR)

static struct mem mem;
static struct cmu cmu;
static int fails;

static void check(const char *what, long got, long want)
{
	printf("%-62s %s\n", what, got == want ? "ok" : "FAIL");
	if (got != want) {
		printf("    got %ld, wanted %ld\n", got, want);
		fails++;
	}
}

static uint32_t rd(uint32_t a)             { return mem_read(&mem, 0x300000u + a, 4); }
static void     wr(uint32_t a, uint32_t v) { mem_write(&mem, 0x300000u + a, 4, v); }

int main(void)
{
	mem_init(&mem);
	cmu_attach(&mem, &cmu);
	check("reset WAKEUPWT auto-wakes clock-switch SLEEP",
	      cmu_slp_auto_wake(&cmu), 1);

	/* Reset state is locked, so a write without unlocking is discarded. */
	wr(0x1b08, GRIFO_CLKCNTL);
	check("write while protected is discarded", rd(0x1b08), 0);
	check("and is counted as blocked", cmu.blocked, 1);

	wr(0x1b24, CMU_PROTECT_OFF);
	wr(0x1b08, GRIFO_CLKCNTL);
	wr(0x1b0c, GRIFO_PLL);
	wr(0x1b14, 1);              /* WAKEUPWT: wait for an interrupt */
	check("write after unlocking takes effect", rd(0x1b08), GRIFO_CLKCNTL);
	check("WAKEUPWT keeps the CPU in SLEEP", cmu_slp_auto_wake(&cmu), 0);
	wr(0x1b14, 0);              /* automatic clock-switch wake */
	check("clearing WAKEUPWT enables automatic wake",
	      cmu_slp_auto_wake(&cmu), 1);

	/* grifo's own arithmetic: 48 MHz / 8 = 6 MHz reference, x10 = 60 MHz. */
	check("PLL reference is 48 MHz / 8", OSC3_HZ / 8, 6000000);
	check("system clock is 60 MHz", cmu_mclk_hz(&cmu), 60000000);
	check("which is the tick timer's 60 counts per microsecond",
	      cmu_mclk_hz(&cmu) / 1000000, 60);

	/* Read-modify-write must preserve bits: CMU_enable1 does |= mask. */
	wr(0x1b04, 0x00000005);
	wr(0x1b04, rd(0x1b04) | 0x00000010);
	check("GATEDCLK1 |= preserves previously enabled clocks",
	      rd(0x1b04), 0x15);

	/* Re-locking blocks again. */
	wr(0x1b24, CMU_PROTECT_ON);
	unsigned long before = cmu.blocked;
	wr(0x1b04, 0xffffffff);
	check("re-locking blocks writes again", cmu.blocked, before + 1);
	check("and the register is unchanged", rd(0x1b04), 0x15);

	/* Selecting a different source changes the derived frequency. */
	wr(0x1b24, CMU_PROTECT_OFF);
	wr(0x1b08, (GRIFO_CLKCNTL & ~(3 << 2)) | OSCSEL_OSC1);
	check("selecting OSC1 gives the 32768 Hz watch crystal",
	      cmu_mclk_hz(&cmu), OSC1_HZ);

	wr(0x1b08, GRIFO_CLKCNTL);
	wr(0x1b0c, GRIFO_PLL & ~PLLPOWR);
	check("selecting an unpowered PLL yields no clock",
	      cmu_mclk_hz(&cmu), 0);

	printf("\n%s\n", fails ? "FAILURES" : "all CMU tests passed");
	return fails != 0;
}
