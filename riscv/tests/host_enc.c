/* host_enc.c - every C33 instruction the translator can emit, in both the
 * bytes it emits and the assembly that means the same thing.
 *
 * jitenc.py feeds the second column to the toolchain's assembler and compares
 * it with the first.  That is the only check on the emitter that does not
 * involve booting something: a wrong field is a different instruction, and a
 * different instruction shows up as a guest that misbehaves somewhere else
 * entirely, minutes later.
 *
 * It exists because of a real one.  The shift-immediate encoding changes shape
 * at a count of sixteen -- `srl %rd,15` and `srl %rd,16` are not one opcode
 * with one field -- and composing it arithmetically was right for every count
 * the disassembly happened to show.  Linux booted into a delay loop it could
 * never leave, and finding out why took six full-system boots.
 */

#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "../rv32.h"
#include "../rv32_jit.h"

void rv32_jit_stub_fault(void) {}
void rv32_jit_stub_indirect(void) {}
void rv32_jit_stub_dev_load(void) {}
void rv32_jit_stub_dev_store(void) {}
void rv32_jit_stub_divop(void) {}
void rv32_jit_stub_csr(void) {}
void rv32_jit_stub_amo(void) {}

#include "../rv32_jit.c"

static uint8_t buf[64];
static struct tc t;

static void start(void)
{
	memset(buf, 0, sizeof buf);
	t.base = buf;
	t.off = 0;
	t.end = sizeof buf;
}

/* One case: what came out, and what it should have been called. */
__attribute__((format(printf, 1, 2)))
static void show(const char *fmt, ...)
{
	unsigned i;
	va_list ap;

	for (i = 0; i < t.off; i += 2)
		printf("%02x%02x", buf[i + 1], buf[i]);
	printf("\t");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
}

static const unsigned regs[] = { 0, 4, 5, 9, 10, 13, 14, 15 };
#define NREGS (sizeof regs / sizeof regs[0])

int main(void)
{
	static const struct { unsigned op; const char *name; } two[] = {
		{ O_MOV, "ld.w %%r%u,%%r%u" },
		{ O_ADD, "add %%r%u,%%r%u" },
		{ O_SUB, "sub %%r%u,%%r%u" },
		{ O_CMP, "cmp %%r%u,%%r%u" },
		{ O_AND, "and %%r%u,%%r%u" },
		{ O_OR,  "or %%r%u,%%r%u" },
		{ O_XOR, "xor %%r%u,%%r%u" },
		{ O_NOT, "not %%r%u,%%r%u" },
		{ O_SRL, "srl %%r%u,%%r%u" },
		{ O_SLL, "sll %%r%u,%%r%u" },
		{ O_SRA, "sra %%r%u,%%r%u" },
		{ O_MLT, "mlt.w %%r%u,%%r%u" },
		{ O_MLTU, "mltu.w %%r%u,%%r%u" },
	};
	static const struct { unsigned op; const char *name; } loads[] = {
		{ O_LDB, "ld.b %%r%u,[%%r%u]" },
		{ O_LDUB, "ld.ub %%r%u,[%%r%u]" },
		{ O_LDH, "ld.h %%r%u,[%%r%u]" },
		{ O_LDUH, "ld.uh %%r%u,[%%r%u]" },
		{ O_LDW, "ld.w %%r%u,[%%r%u]" },
	};
	static const struct { unsigned op; const char *name; } stores[] = {
		{ O_STB, "ld.b [%%r%u],%%r%u" },
		{ O_STH, "ld.h [%%r%u],%%r%u" },
		{ O_STW, "ld.w [%%r%u],%%r%u" },
	};
	static const struct { unsigned op; const char *name; } imm[] = {
		{ I_CMP, "xcmp %%r%u,%d" },
		{ I_MOV, "xld.w %%r%u,%d" },
		{ I_AND, "xand %%r%u,%d" },
		{ I_OR,  "xoor %%r%u,%d" },
		{ I_XOR, "xxor %%r%u,%d" },
	};
	/* Values on and either side of every width the prefixes compose. */
	static const int32_t values[] = {
		0, 1, -1, 31, 32, -32, -33, 63, 64, 255, -256,
		4095, 4096, 262143, 262144, -262144, -262145,
		0x7fffff, -0x800000, 0x12345678, (int32_t)0x80000000, -1000000
	};
	unsigned i, a, b;

	for (i = 0; i < sizeof two / sizeof two[0]; ++i)
		for (a = 0; a < NREGS; ++a)
			for (b = 0; b < NREGS; ++b) {
				start();
				rr(&t, two[i].op, regs[a], regs[b]);
				show(two[i].name, regs[a], regs[b]);
			}
	for (i = 0; i < sizeof loads / sizeof loads[0]; ++i)
		for (a = 0; a < NREGS; ++a)
			for (b = 0; b < NREGS; ++b) {
				start();
				rr(&t, loads[i].op, regs[a], regs[b]);
				show(loads[i].name, regs[a], regs[b]);
			}
	for (i = 0; i < sizeof stores / sizeof stores[0]; ++i)
		for (a = 0; a < NREGS; ++a)
			for (b = 0; b < NREGS; ++b) {
				start();
				rr(&t, stores[i].op, regs[a], regs[b]);
				show(stores[i].name, regs[b], regs[a]);
			}
	for (a = 0; a < NREGS; ++a) {
		start();
		rr(&t, O_SPEC, regs[a], 2);
		show("ld.w %%r%u,%%alr", regs[a]);
		start();
		rr(&t, O_SPEC, regs[a], 3);
		show("ld.w %%r%u,%%ahr", regs[a]);
	}

	/* Immediates, including every prefix count. */
	for (i = 0; i < sizeof imm / sizeof imm[0]; ++i)
		for (a = 0; a < NREGS; ++a)
			for (b = 0; b < sizeof values / sizeof values[0]; ++b) {
				start();
				ri(&t, imm[i].op, regs[a], (uint32_t)values[b]);
				show(imm[i].name, regs[a], values[b]);
			}
	for (a = 0; a < NREGS; ++a)
		for (b = 0; b < sizeof values / sizeof values[0]; ++b) {
			int32_t v = values[b];

			start();
			addimm(&t, regs[a], (uint32_t)v, 0);
			if (v < 0)
				show("xsub %%r%u,%u", regs[a], (unsigned)-v);
			else
				show("xadd %%r%u,%u", regs[a], (unsigned)v);
			start();
			addimm(&t, regs[a], (uint32_t)v, 1);
			if (v < 0)
				show("xadd %%r%u,%u", regs[a], (unsigned)-v);
			else
				show("xsub %%r%u,%u", regs[a], (unsigned)v);
		}

	/* The shifts, whose count is not an ordinary immediate. */
	for (a = 0; a < NREGS; ++a)
		for (b = 0; b < 32; ++b) {
			start();
			shift(&t, S_SRL, regs[a], b);
			show("srl %%r%u,%u", regs[a], b);
			start();
			shift(&t, S_SLL, regs[a], b);
			show("sll %%r%u,%u", regs[a], b);
			start();
			shift(&t, S_SRA, regs[a], b);
			show("sra %%r%u,%u", regs[a], b);
			start();
			shift(&t, S_RR, regs[a], b);
			show("rr %%r%u,%u", regs[a], b);
		}

	/* The guest register file, addressed by a prefix. */
	for (a = 0; a < 32; ++a) {
		start();
		getx(&t, R4, a);
		if (a)
			show("xld.w %%r4,[%%r0+%u]", a * 4);
		else
			show("ld.w %%r4,0");
		if (a) {
			start();
			putx(&t, a, R5);
			show("xld.w [%%r0+%u],%%r5", a * 4);
		}
	}
	return 0;
}
