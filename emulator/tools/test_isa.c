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

static void jumps(void)
{
	struct c33 c;

	printf("\njpr/jpr.d\n");
	c = init(0x02c1);                    /* jpr %r1 */
	c.r[1] = 0x20;
	c33_step(&c);
	check("jpr adds its register to the instruction PC", c.pc, ENTRY + 0x20);
	c = init(0x03c1);                    /* jpr.d %r1 */
	c.r[1] = 0x20;
	tw(NULL, ENTRY + 2, 2, 0x0000);      /* nop delay slot */
	c33_step(&c);
	check("jpr.d first selects its delay slot", c.pc, ENTRY + 2);
	c33_step(&c);
	check("jpr.d branches after the slot", c.pc, ENTRY + 0x20);

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

int main(void)
{
	arithmetic();
	swaps();
	jumps();
	if (fails) {
		printf("\nFAILURES: %d\n", fails);
		return 1;
	}
	printf("\nall extra ISA tests passed\n");
	return 0;
}
