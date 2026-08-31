/* C33 PE instructions which the four firmware images do not exercise. */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/c33.h"

#define ENTRY 0x100u

static uint8_t ram[0x400];
static int fails;

static uint32_t tr(void *ctx, uint32_t a, unsigned sz)
{
	uint32_t v = 0;
	for (unsigned i = 0; i < sz; i++)
		v |= (uint32_t)ram[(a + i) & 0x3ff] << (8 * i);
	return v;
}

static void tw(void *ctx, uint32_t a, unsigned sz, uint32_t v)
{
	for (unsigned i = 0; i < sz; i++)
		ram[(a + i) & 0x3ff] = (uint8_t)(v >> (8 * i));
}

static void check(const char *what, uint32_t got, uint32_t want)
{
	printf("%-58s %s\n", what, got == want ? "ok" : "FAIL");
	if (got != want) {
		printf("    got 0x%08x, wanted 0x%08x\n", got, want);
		fails++;
	}
}

static struct c33 init(uint16_t insn)
{
	struct c33 c;
	memset(&c, 0, sizeof c);
	memset(ram, 0, sizeof ram);
	c.bus.read = tr;
	c.bus.write = tw;
	c33_reset(&c, ENTRY);
	tw(NULL, ENTRY, 2, insn);
	return c;
}

static bool auto_wake(void *ctx)
{
	return *(bool *)ctx;
}

static void sleep_modes(void)
{
	struct c33 c;
	bool automatic;

	printf("\nslp\n");
	c = init(0x0040);                    /* slp */
	c33_step(&c);
	check("core slp waits without an SoC auto-wake", c.sleeping, 1);
	c = init(0x0040);
	automatic = true;
	c.slp_auto_wake = auto_wake;
	c.slp_ctx = &automatic;
	c33_step(&c);
	check("clock-switch slp resumes automatically", c.sleeping, 0);
	c = init(0x0040);
	automatic = false;
	c.slp_auto_wake = auto_wake;
	c.slp_ctx = &automatic;
	c33_step(&c);
	check("interrupt-wait slp remains asleep", c.sleeping, 1);
}

static void arithmetic(void)
{
	struct c33 c;

	printf("\nadc/sbc\n");
	c = init(0xb810);                    /* adc %r0,%r1 */
	c.r[0] = 0xffffffff; c.r[1] = 0; c.sr[SR_PSR] = PSR_C;
	c33_step(&c);
	check("adc consumes carry-in", c.r[0], 0);
	check("adc reports carry and zero", c.sr[SR_PSR], PSR_C | PSR_Z);
	c = init(0xb810);
	c.r[0] = 0x7fffffff; c.r[1] = 0; c.sr[SR_PSR] = PSR_C;
	c33_step(&c);
	check("adc signed-overflow result", c.r[0], 0x80000000);
	check("adc reports overflow and negative", c.sr[SR_PSR], PSR_V | PSR_N);
	c = init(0xb810);
	c.r[0] = 0x80000000; c.r[1] = 0x7fffffff; c.sr[SR_PSR] = PSR_C;
	c33_step(&c);
	check("adc handles wrapped second operand", c.r[0], 0);
	check("adc wrapped operand flags", c.sr[SR_PSR], PSR_C | PSR_Z);

	c = init(0xbc10);                    /* sbc %r0,%r1 */
	c.r[0] = 0; c.r[1] = 0; c.sr[SR_PSR] = PSR_C;
	c33_step(&c);
	check("sbc consumes borrow-in", c.r[0], 0xffffffff);
	check("sbc reports borrow and negative", c.sr[SR_PSR], PSR_C | PSR_N);
	c = init(0xbc10);
	c.r[0] = 0x80000000; c.r[1] = 0; c.sr[SR_PSR] = PSR_C;
	c33_step(&c);
	check("sbc signed-overflow result", c.r[0], 0x7fffffff);
	check("sbc reports overflow", c.sr[SR_PSR], PSR_V);
	c = init(0xbc10);
	c.r[0] = 0x7fffffff; c.r[1] = 0xffffffff; c.sr[SR_PSR] = PSR_C;
	c33_step(&c);
	check("sbc handles wrapped subtrahend", c.r[0], 0x7fffffff);
	check("sbc wrapped subtrahend flags", c.sr[SR_PSR], PSR_C);
}

static struct c33 run_extended_register(uint16_t insn, uint32_t rd,
					uint32_t rs)
{
	struct c33 c = init(0xc001);          /* ext 1 */

	tw(NULL, ENTRY + 2, 2, insn);
	c.r[0] = rd;
	c.r[1] = rs;
	c33_step(&c);
	c33_step(&c);
	return c;
}

static void extended_register_forms(void)
{
	struct c33 c;

	printf("\nextended register forms\n");
	c = run_extended_register(0x2210, 99, 42); /* add %r0,%r1 */
	check("ext; add uses rs + immediate", c.r[0], 43);
	c = run_extended_register(0x2610, 99, 42); /* sub %r0,%r1 */
	check("ext; sub uses rs - immediate", c.r[0], 41);
	c = run_extended_register(0x2a10, 1, 2);  /* cmp %r0,%r1 */
	check("ext; cmp compares rs with immediate", c.sr[SR_PSR] & 0xf,
	      0);
	check("ext; cmp does not change rd", c.r[0], 1);
	c = run_extended_register(0x3210, 99, 6); /* and %r0,%r1 */
	check("ext; and uses rs and immediate", c.r[0], 0);
	c = run_extended_register(0x3610, 99, 6); /* or %r0,%r1 */
	check("ext; or uses rs and immediate", c.r[0], 7);
	c = run_extended_register(0x3a10, 99, 6); /* xor %r0,%r1 */
	check("ext; xor uses rs and immediate", c.r[0], 7);

	/* The manual marks ext unusable for both SP arithmetic forms. */
	c = init(0xc001);                       /* ext 1 */
	tw(NULL, ENTRY + 2, 2, 0x8001);        /* add %sp,1 */
	c.sr[SR_SP] = 0x200;
	c33_step(&c);
	c33_step(&c);
	check("ext is a nop before add %sp,imm10", c.sr[SR_SP], 0x204);
	c = init(0xc001);
	tw(NULL, ENTRY + 2, 2, 0x8401);        /* sub %sp,1 */
	c.sr[SR_SP] = 0x200;
	c33_step(&c);
	c33_step(&c);
	check("ext is a nop before sub %sp,imm10", c.sr[SR_SP], 0x1fc);
}

static void swaps(void)
{
	struct c33 c;

	printf("\nswap/swaph\n");
	c = init(0x9210);                    /* swap %r0,%r1 */
	c.r[1] = 0x87654321;
	c33_step(&c);
	check("swap reverses all four bytes", c.r[0], 0x21436587);
	c = init(0x9a10);                    /* swaph %r0,%r1 */
	c.r[1] = 0x87654321;
	c33_step(&c);
	check("swaph swaps bytes within halfwords", c.r[0], 0x65872143);
}

static void multiply(void)
{
	struct c33 c;

	printf("\nmultiply\n");
	c = init(0xa210);                    /* mlt.h %r0,%r1 */
	c.r[0] = 0xfffffffe;
	c.r[1] = 3;
	c.sr[SR_AHR] = 0x12345678;
	c.sr[SR_PSR] = PSR_C | PSR_V;
	c33_step(&c);
	check("mlt.h writes signed product to ALR", c.sr[SR_ALR], 0xfffffffa);
	check("mlt.h leaves AHR unchanged", c.sr[SR_AHR], 0x12345678);
	check("mlt.h leaves flags unchanged", c.sr[SR_PSR], PSR_C | PSR_V);

	c = init(0xa610);                    /* mltu.h %r0,%r1 */
	c.r[0] = 0xfffffffe;
	c.r[1] = 3;
	c.sr[SR_AHR] = 0x87654321;
	c33_step(&c);
	check("mltu.h writes unsigned product to ALR", c.sr[SR_ALR], 0x2fffa);
	check("mltu.h leaves AHR unchanged", c.sr[SR_AHR], 0x87654321);

	c = init(0xaa10);                    /* mlt.w %r0,%r1 */
	c.r[0] = 0xfffffffe;
	c.r[1] = 3;
	c33_step(&c);
	check("mlt.w writes signed product low word", c.sr[SR_ALR], 0xfffffffa);
	check("mlt.w writes signed product high word", c.sr[SR_AHR], 0xffffffff);

	c = init(0xae10);                    /* mltu.w %r0,%r1 */
	c.r[0] = 0xfffffffe;
	c.r[1] = 3;
	c33_step(&c);
	check("mltu.w writes unsigned product low word", c.sr[SR_ALR], 0xfffffffa);
	check("mltu.w writes unsigned product high word", c.sr[SR_AHR], 2);
}

static void special_stack(void)
{
	struct c33 c;

	printf("\npushs/pops\n");
	c = init(0x0093);                    /* pushs %ahr */
	c.sr[SR_SP] = 0x300;
	c.sr[SR_ALR] = 0x11111111;
	c.sr[SR_AHR] = 0x22222222;
	c33_step(&c);
	check("pushs %ahr saves both registers", c.sr[SR_SP], 0x2f8);
	check("pushs leaves ALR at the new stack top",
	      tr(NULL, 0x2f8, 4), 0x11111111);
	check("pushs saves AHR above ALR", tr(NULL, 0x2fc, 4), 0x22222222);
	check("pushs %ahr takes three clocks", c.clk, 3);

	c = init(0x00d3);                    /* pops %ahr */
	c.sr[SR_SP] = 0x300;
	tw(NULL, 0x300, 4, 0x33333333);
	tw(NULL, 0x304, 4, 0x44444444);
	c33_step(&c);
	check("pops %ahr restores ALR", c.sr[SR_ALR], 0x33333333);
	check("pops %ahr restores AHR", c.sr[SR_AHR], 0x44444444);
	check("pops %ahr consumes two words", c.sr[SR_SP], 0x308);
	check("pops %ahr takes three clocks", c.clk, 3);

	c = init(0x0094);                    /* invalid pushs %lco */
	c.sr[SR_SP] = 0x300;
	c.sr[SR_LCO] = 0x55555555;
	c33_step(&c);
	check("pushs of a non-ALR/AHR register is a no-op", c.sr[SR_SP], 0x300);
	check("invalid pushs writes no stack data", tr(NULL, 0x2fc, 4), 0);

	c = init(0x00d4);                    /* invalid pops %lco */
	c.sr[SR_SP] = 0x300;
	c.sr[SR_LCO] = 0x55555555;
	tw(NULL, 0x300, 4, 0xaaaaaaaa);
	c33_step(&c);
	check("pops of a non-ALR/AHR register is a no-op", c.sr[SR_SP], 0x300);
	check("invalid pops leaves its named register alone",
	      c.sr[SR_LCO], 0x55555555);
}

static void memory_timing(void)
{
	static const struct {
		uint16_t insn;
		const char *name;
	} two_clock[] = {
		{ 0x2110, "ld.b %r0,[%r1]+" },
		{ 0x3510, "ld.b [%r1]+,%r0" },
		{ 0x4000, "ld.b %r0,[%sp+0]" },
		{ 0x5400, "ld.b [%sp+0],%r0" },
		{ 0x3110, "ld.w %r0,[%r1]+" },
		{ 0x3d10, "ld.w [%r1]+,%r0" },
		{ 0x5000, "ld.w %r0,[%sp+0]" },
		{ 0x5c00, "ld.w [%sp+0],%r0" },
	};
	struct c33 c;
	char what[80];

	printf("\nmemory timing\n");
	for (unsigned i = 0; i < sizeof two_clock / sizeof two_clock[0]; i++) {
		c = init(two_clock[i].insn);
		c.r[1] = 0x200;
		c.sr[SR_SP] = 0x200;
		c33_step(&c);
		snprintf(what, sizeof what, "%s takes two clocks", two_clock[i].name);
		check(what, c.clk, 2);
	}
}

static void jumps(void)
{
	struct c33 c;

	printf("\njpr/jpr.d\n");
	c = init(0x02c1);                    /* jpr %r1 */
	c.r[1] = 0x21;
	c33_step(&c);
	check("jpr ignores the register's low bit", c.pc, ENTRY + 0x20);
	c = init(0x03c1);                    /* jpr.d %r1 */
	c.r[1] = 0x21;
	tw(NULL, ENTRY + 2, 2, 0x0000);      /* nop delay slot */
	c33_step(&c);
	check("jpr.d first selects its delay slot", c.pc, ENTRY + 2);
	c33_step(&c);
	check("jpr.d ignores the register's low bit", c.pc, ENTRY + 0x20);

	c = init(0x0681);                    /* jp %r1 */
	c.r[1] = 0x241;
	c33_step(&c);
	check("jp ignores the register's low bit", c.pc, 0x240);
	c = init(0x0601);                    /* call %r1 */
	c.r[1] = 0x241;
	c.sr[SR_SP] = 0x300;
	c33_step(&c);
	check("call ignores the register's low bit", c.pc, 0x240);
	check("call still saves its even return address",
	      tr(NULL, 0x2fc, 4), ENTRY + 2);

	c = init(0x0740);                    /* ret.d */
	c.sr[SR_SP] = 0x300;
	tw(NULL, 0x300, 4, 0x240);
	tw(NULL, ENTRY + 2, 2, 0x0000);      /* nop delay slot */
	c33_step(&c);
	check("ret.d pops its return address", c.sr[SR_SP], 0x304);
	check("ret.d first selects its delay slot", c.pc, ENTRY + 2);
	c33_step(&c);
	check("ret.d returns after the slot", c.pc, 0x240);

	c = init(0x0440);                    /* retd */
	tw(NULL, 0x6000c, 4, 0x12345678);
	tw(NULL, 0x60008, 4, 0x240);
	c33_step(&c);
	check("retd restores R0 from the debug save area", c.r[0], 0x12345678);
	check("retd restores PC from the debug save area", c.pc, 0x240);
}

static void debug_exception(void)
{
	struct c33 c = init(0x0400);          /* brk */

	printf("\nbrk/retd\n");
	c.r[0] = 0x12345678;
	c.sr[SR_PSR] = PSR_IE;
	tw(NULL, 0x60000, 4, 0x200);         /* debug vector */
	tw(NULL, 0x200, 2, 0x0440);          /* retd */
	tw(NULL, ENTRY + 2, 2, 0x0000);      /* resumed nop */
	c33_step(&c);
	check("brk saves the following PC", tr(NULL, 0x60008, 4), ENTRY + 2);
	check("brk saves R0", tr(NULL, 0x6000c, 4), 0x12345678);
	check("brk loads the fixed debug vector", c.pc, 0x200);
	check("brk enters debug mode", c.debug_mode, 1);
	check("brk takes its documented nine clocks", c.clk, 9);

	/* Normal interrupts are not accepted inside a debug exception. */
	c.sr[SR_TTBR] = 0x300;
	tw(NULL, 0x304, 4, 0x280);
	tw(NULL, 0x280, 2, 0x0000);
	c33_raise_irq(&c, 1, 7);
	c33_step(&c);                         /* execute retd, not the IRQ */
	check("interrupt stays pending through retd", c.irqs_taken, 0);
	check("retd leaves debug mode", c.debug_mode, 0);
	check("retd returns to the saved PC", c.pc, ENTRY + 2);
	c33_step(&c);                         /* IRQ can now be accepted */
	check("pending interrupt is accepted after retd", c.irqs_taken, 1);
}

int main(void)
{
	arithmetic();
	extended_register_forms();
	multiply();
	swaps();
	special_stack();
	memory_timing();
	jumps();
	debug_exception();
	sleep_modes();
	if (fails) {
		printf("\nFAILURES: %d\n", fails);
		return 1;
	}
	printf("\nall extra ISA tests passed\n");
	return 0;
}
