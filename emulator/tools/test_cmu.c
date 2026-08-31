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
#define MCLKDIV       (1 << 12)
#define OSCSEL_PLL    (3 << 2)
#define OSCSEL_OSC3   (2 << 2)
#define OSCSEL_OSC1   (1 << 2)
#define SOSC3         (1 << 1)
#define SOSC1         (1 << 0)

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
	check("GATEDCLK0 has its documented reset value", rd(0x1b00),
	      0x00000008);
	check("GATEDCLK1 has its documented reset value", rd(0x1b04),
	      0x3f0fffff);
	check("CLKCNTL has its documented reset value", rd(0x1b08),
	      0x00770003);
	check("PLL has its documented reset value", rd(0x1b0c),
	      0x00101804);
	check("SSCG has its documented reset value", rd(0x1b10),
	      0x0000f000);
	check("reset clock source is the 48 MHz OSC3", cmu_mclk_hz(&cmu),
	      OSC3_HZ);
	check("reset WAKEUPWT auto-wakes clock-switch SLEEP",
	      cmu_slp_auto_wake(&cmu), 1);
	for (unsigned channel = 0; channel < 6; channel++) {
		char label[80];
		snprintf(label, sizeof label, "timer %u clock is supplied after reset",
			 channel);
		check(label, cmu_t16_enabled(&cmu, channel), 1);
	}

	/* Reset state is locked, so a write without unlocking is discarded. */
	wr(0x1b08, GRIFO_CLKCNTL);
	check("write while protected is discarded", rd(0x1b08), 0x00770003);
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
	wr(0x1b04, 1u << 15);       /* only TM2_CKE */
	check("TM2_CKE supplies timer 2", cmu_t16_enabled(&cmu, 2), 1);
	check("a cleared TM0_CKE gates timer 0", cmu_t16_enabled(&cmu, 0), 0);

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

	/* A board power cycle resets both register contents and protection. */
	cmu_reset(&cmu);
	check("power cycle restores GATEDCLK1", rd(0x1b04), 0x3f0fffff);
	wr(0x1b04, 0);
	check("power cycle restores write protection", rd(0x1b04), 0x3f0fffff);

	/* Selecting a different source changes the derived frequency. */
	wr(0x1b24, CMU_PROTECT_OFF);
	wr(0x1b08, (GRIFO_CLKCNTL & ~(3 << 2)) | OSCSEL_OSC1 | SOSC1);
	check("selecting OSC1 gives the 32768 Hz watch crystal",
	      cmu_mclk_hz(&cmu), OSC1_HZ);

	wr(0x1b08, OSCSEL_OSC3 | SOSC3 | OSC3DIV_32);
	check("OSC3DIV divides the 48 MHz crystal by 32",
	      cmu_mclk_hz(&cmu), OSC3_HZ / 32);
	wr(0x1b08, OSCSEL_OSC3 | SOSC3 | OSC3DIV_32 | MCLKDIV);
	check("MCLKDIV halves the selected system clock",
	      cmu_mclk_hz(&cmu), OSC3_HZ / 64);
	wr(0x1b08, OSCSEL_OSC3 | OSC3DIV_32);
	check("a stopped OSC3 cannot drive MCLK", cmu_mclk_hz(&cmu), 0);
	wr(0x1b08, OSCSEL_OSC1);
	check("a stopped OSC1 cannot drive MCLK", cmu_mclk_hz(&cmu), 0);

	wr(0x1b08, GRIFO_CLKCNTL);
	wr(0x1b0c, GRIFO_PLL & ~PLLPOWR);
	check("selecting an unpowered PLL yields no clock",
	      cmu_mclk_hz(&cmu), 0);

	printf("\n%s\n", fails ? "FAILURES" : "all CMU tests passed");
	return fails != 0;
}
