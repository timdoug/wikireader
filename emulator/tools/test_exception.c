/* Synchronous exceptions from C33 PE Core Manual sections 6.3.3-6.3.10. */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/c33.h"

#define TTBR    0x400u
#define ENTRY   0x1000u
#define HANDLER 0x9000u
#define STACK   0x10000u

static uint8_t ram[0x20000];
static int fails;

static uint32_t tr(void *ctx, uint32_t a, unsigned sz)
{
	uint32_t v = 0;
	for (unsigned i = 0; i < sz; i++)
		v |= (uint32_t)ram[(a + i) & 0x1ffff] << (8 * i);
	return v;
}

static void tw(void *ctx, uint32_t a, unsigned sz, uint32_t v)
{
	for (unsigned i = 0; i < sz; i++)
		ram[(a + i) & 0x1ffff] = (uint8_t)(v >> (8 * i));
}

static bool disabled(void *ctx, unsigned vector)
{
	return false;
}

static void check(const char *what, uint32_t got, uint32_t want)
{
	printf("%-58s %s\n", what, got == want ? "ok" : "FAIL");
	if (got != want) {
		printf("    got 0x%08x, wanted 0x%08x\n", got, want);
		fails++;
	}
}

static void test_reset_registers(void)
{
	struct c33 c = {0};

	c33_reset(&c, ENTRY);
	printf("\ncold reset registers\n");
	check("caller-selected entry is loaded into PC", c.pc, ENTRY);
	check("PSR reset value", c.sr[SR_PSR], 0);
	check("TTBR reset value", c.sr[SR_TTBR], 0x00c00000);
	check("IDIR identifies a C33 PE core", c.sr[SR_IDIR], 0x06000000);
	check("DBBR fixed value", c.sr[SR_DBBR], 0x00060000);
}

static struct c33 run_sreg(uint16_t insn, uint32_t r0)
{
	struct c33 c;

	memset(&c, 0, sizeof c);
	memset(ram, 0, sizeof ram);
	c.bus.read = tr;
	c.bus.write = tw;
	c33_reset(&c, ENTRY);
	c.r[0] = r0;
	tw(NULL, ENTRY, 2, insn);
	c33_step(&c);
	return c;
}

static void test_special_registers(void)
{
	struct c33 c;

	printf("\nspecial-register constraints\n");
	c = run_sreg(0xa4f0, 0);             /* ld.w %r0,%pc */
	check("PC transfer reads the following address", c.r[0], ENTRY + 2);
	c = run_sreg(0xa000, ~0u);           /* ld.w %psr,%r0 */
	check("unused PSR bits remain zero", c.sr[SR_PSR],
	      PSR_IL_MASK | PSR_IE | PSR_C | PSR_V | PSR_Z | PSR_N);
	c = run_sreg(0xa001, 0x1234567b);     /* ld.w %sp,%r0 */
	check("SP remains word aligned", c.sr[SR_SP], 0x12345678);
	c = run_sreg(0xa008, 0x123457ff);     /* ld.w %ttbr,%r0 */
	check("TTBR remains 1K aligned", c.sr[SR_TTBR], 0x12345400);
	c = run_sreg(0xa00a, ~0u);           /* ld.w %idir,%r0 */
	check("IDIR is read-only", c.sr[SR_IDIR], 0x06000000);
	c = run_sreg(0xa00b, ~0u);           /* ld.w %dbbr,%r0 */
	check("DBBR is read-only", c.sr[SR_DBBR], 0x00060000);
	c = run_sreg(0xa00f, 0x12345678);     /* ld.w %pc,%r0 */
	check("PC is read-only", c.pc, ENTRY + 2);
}

static struct c33 init(unsigned vector)
{
	struct c33 c;
	memset(&c, 0, sizeof c);
	memset(ram, 0, sizeof ram);
	c.bus.read = tr;
	c.bus.write = tw;
	c.irq_enabled = disabled;
	c33_reset(&c, ENTRY);
	c.sr[SR_TTBR] = TTBR;
	c.sr[SR_SP] = STACK;
	c.sr[SR_PSR] = PSR_IE | (15u << PSR_IL_SHIFT);
	c.sr[SR_IDIR] = 0x06120000;
	tw(NULL, TTBR + vector * 4, 4, HANDLER);
	tw(NULL, HANDLER, 2, 0x0000);       /* nop */
	return c;
}

static void check_frame(struct c33 *c, uint32_t return_pc)
{
	check("exception loads its vector", c->pc, HANDLER);
	check("exception pushes an eight-byte frame", c->sr[SR_SP], STACK - 8);
	check("saved PC has the documented value",
	      tr(NULL, STACK - 4, 4), return_pc);
	check("saved PSR retains IE and IL",
	      tr(NULL, STACK - 8, 4), PSR_IE | (15u << PSR_IL_SHIFT));
	check("exception entry clears IE", c->sr[SR_PSR] & PSR_IE, 0);
	check("synchronous exception does not rewrite IL",
	      c->sr[SR_PSR] & PSR_IL_MASK, 15u << PSR_IL_SHIFT);
	check("interrupt-controller mask cannot block an exception",
	      c->pc, HANDLER);
}

static void test_undefined(uint16_t insn, const char *name)
{
	struct c33 c = init(3);
	tw(NULL, ENTRY, 2, insn);
	c33_step(&c);
	printf("\nundefined: %s (0x%04x)\n", name, insn);
	check_frame(&c, ENTRY + 2);
	check("IDIR retains the PE type/revision half",
	      c.sr[SR_IDIR] & 0xffff0000u, 0x06120000);
	check("IDIR records the undefined instruction",
	      c.sr[SR_IDIR] & 0xffffu, insn);
	check("undefined instruction is not a host fault", c.halted, 0);
}

static void test_ext(void)
{
	struct c33 c = init(2);
	tw(NULL, ENTRY + 0, 2, 0xc200);     /* ext 0x200 */
	tw(NULL, ENTRY + 2, 2, 0xc353);     /* ext 0x353 */
	tw(NULL, ENTRY + 4, 2, 0xc001);     /* illegal third ext */
	c33_step(&c);
	c33_step(&c);
	c33_step(&c);
	printf("\nthird ext\n");
	check_frame(&c, ENTRY);
	check("ext state is discarded on the exception", c.n_ext, 0);
}

static void test_misaligned(void)
{
	struct c33 c = init(6);
	c.r[0] = 0xfeedface;
	c.r[1] = 0x1001;
	tw(NULL, ENTRY, 2, 0x3010);         /* ld.w %r0,[%r1] */
	c33_step(&c);
	printf("\nmisaligned load\n");
	check_frame(&c, ENTRY);
	check("faulting load does not alter its destination", c.r[0], 0xfeedface);
	check("misalignment is not a host fault", c.halted, 0);
}

int main(void)
{
	static const struct {
		uint16_t word;
		const char *name;
	} undefined[] = {
		{ 0x0300, "invalid encoding" },
		{ 0x8b10, "div0s" }, { 0x8f10, "div0u" },
		{ 0x9310, "div1" },  { 0x9710, "div2s" },
		{ 0x9b00, "div3s" }, { 0xb210, "mac" },
		{ 0x9610, "mirror" }, { 0x8a10, "scan0" },
		{ 0x8e10, "scan1" },
	};

	test_reset_registers();
	test_special_registers();
	for (unsigned i = 0; i < sizeof undefined / sizeof undefined[0]; i++)
		test_undefined(undefined[i].word, undefined[i].name);
	test_ext();
	test_misaligned();

	printf("\n%s\n", fails ? "FAILURES" : "all exception tests passed");
	return fails != 0;
}
