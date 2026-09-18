/* host_jit.c - translate a guest program on the build machine and show the
 * C33 instructions that came out.
 *
 * The translator emits machine code by hand, so the one thing that cannot be
 * caught by running it is a wrong encoding: a bad field lands as some other
 * instruction and the failure appears somewhere else entirely, minutes into a
 * boot.  This puts the bytes through the toolchain's own disassembler instead,
 * where a wrong field is visible immediately -- and it runs in a second rather
 * than the nine minutes a full-system boot takes.
 *
 *   make jitdis          the whole corpus, disassembled
 *
 * It is a reading test, not an assertion test.  What it is read against is
 * rv32_jit.c's comments about what each guest instruction should become.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../rv32.h"
#include "../rv32_jit.h"

/* The runtime lives in rv32_jitrt.s and there is no C33 here to run it.  Only
   the addresses matter, and only for displacements the disassembler will show
   as nonsense either way. */
void rv32_jit_stub_fault(void) {}
void rv32_jit_stub_indirect(void) {}
void rv32_jit_stub_dev_load(void) {}
void rv32_jit_stub_dev_store(void) {}
void rv32_jit_stub_divop(void) {}
void rv32_jit_stub_csr(void) {}
void rv32_jit_stub_amo(void) {}

#include "../rv32_jit.c"

/* rv32 encodings, built rather than assembled: this file has to run wherever
   the tree builds and a RISC-V assembler is not part of that. */
static uint32_t r_type(unsigned f7, unsigned rs2, unsigned rs1, unsigned f3,
		       unsigned rd, unsigned op)
{
	return (f7 << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) |
	       (rd << 7) | op;
}
static uint32_t i_type(int imm, unsigned rs1, unsigned f3, unsigned rd,
		       unsigned op)
{
	return ((uint32_t)imm << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op;
}
static uint32_t s_type(int imm, unsigned rs2, unsigned rs1, unsigned f3,
		       unsigned op)
{
	return (((uint32_t)imm >> 5) << 25) | (rs2 << 20) | (rs1 << 15) |
	       (f3 << 12) | ((imm & 0x1f) << 7) | op;
}
static uint32_t b_type(int imm, unsigned rs2, unsigned rs1, unsigned f3,
		       unsigned op)
{
	uint32_t u = (uint32_t)imm;

	return (((u >> 12) & 1) << 31) | (((u >> 5) & 0x3f) << 25) |
	       (rs2 << 20) | (rs1 << 15) | (f3 << 12) |
	       (((u >> 1) & 0xf) << 8) | (((u >> 11) & 1) << 7) | op;
}
static uint32_t u_type(uint32_t imm, unsigned rd, unsigned op)
{
	return (imm & 0xfffff000u) | (rd << 7) | op;
}
static uint32_t j_type(int imm, unsigned rd, unsigned op)
{
	uint32_t u = (uint32_t)imm;

	return (((u >> 20) & 1) << 31) | (((u >> 1) & 0x3ff) << 21) |
	       (((u >> 11) & 1) << 20) | (((u >> 12) & 0xff) << 12) |
	       (rd << 7) | op;
}

/* The disassembler shows symbols, not comments, so each piece is given one. */
static void label(const char *what)
{
	for (; *what; ++what)
		putchar((*what >= 'a' && *what <= 'z') ||
			(*what >= '0' && *what <= '9') ? *what : '_');
}

struct piece {
	const char *what;
	uint32_t ir[8];
	unsigned n;
};

/* Each piece is translated on its own so that the disassembly can be read
   against one guest instruction at a time.  They end in a branch because a
   trace has to end somewhere, and a backward one so nothing is laid out after
   it. */
#define CORPUS \
{ \
	{ "lui  x5,0x12345000",   { u_type(0x12345000, 5, 0x37) }, 1 }, \
	{ "auipc x5,0x1000",      { u_type(0x1000, 5, 0x17) }, 1 }, \
	{ "addi x5,x6,-3",        { i_type(-3, 6, 0, 5, 0x13) }, 1 }, \
	{ "addi x5,x0,1000",      { i_type(1000, 0, 0, 5, 0x13) }, 1 }, \
	{ "slti x5,x6,7",         { i_type(7, 6, 2, 5, 0x13) }, 1 }, \
	{ "sltiu x5,x6,7",        { i_type(7, 6, 3, 5, 0x13) }, 1 }, \
	{ "xori x5,x6,-1",        { i_type(-1, 6, 4, 5, 0x13) }, 1 }, \
	{ "ori  x5,x6,255",       { i_type(255, 6, 6, 5, 0x13) }, 1 }, \
	{ "andi x5,x6,15",        { i_type(15, 6, 7, 5, 0x13) }, 1 }, \
	{ "slli x5,x6,3",         { i_type(3, 6, 1, 5, 0x13) }, 1 }, \
	{ "srli x5,x6,3",         { i_type(3, 6, 5, 5, 0x13) }, 1 }, \
	{ "srai x5,x6,3",         { i_type(0x403, 6, 5, 5, 0x13) }, 1 }, \
	{ "add  x5,x6,x7",        { r_type(0, 7, 6, 0, 5, 0x33) }, 1 }, \
	{ "sub  x5,x6,x7",        { r_type(0x20, 7, 6, 0, 5, 0x33) }, 1 }, \
	{ "sll  x5,x6,x7",        { r_type(0, 7, 6, 1, 5, 0x33) }, 1 }, \
	{ "slt  x5,x6,x7",        { r_type(0, 7, 6, 2, 5, 0x33) }, 1 }, \
	{ "sltu x5,x6,x7",        { r_type(0, 7, 6, 3, 5, 0x33) }, 1 }, \
	{ "xor  x5,x6,x7",        { r_type(0, 7, 6, 4, 5, 0x33) }, 1 }, \
	{ "srl  x5,x6,x7",        { r_type(0, 7, 6, 5, 5, 0x33) }, 1 }, \
	{ "sra  x5,x6,x7",        { r_type(0x20, 7, 6, 5, 5, 0x33) }, 1 }, \
	{ "or   x5,x6,x7",        { r_type(0, 7, 6, 6, 5, 0x33) }, 1 }, \
	{ "and  x5,x6,x7",        { r_type(0, 7, 6, 7, 5, 0x33) }, 1 }, \
	{ "mul  x5,x6,x7",        { r_type(1, 7, 6, 0, 5, 0x33) }, 1 }, \
	{ "mulh x5,x6,x7",        { r_type(1, 7, 6, 1, 5, 0x33) }, 1 }, \
	{ "mulhu x5,x6,x7",       { r_type(1, 7, 6, 3, 5, 0x33) }, 1 }, \
	{ "divu x5,x6,x7",        { r_type(1, 7, 6, 5, 5, 0x33) }, 1 }, \
	{ "lb   x5,4(x6)",        { i_type(4, 6, 0, 5, 0x03) }, 1 }, \
	{ "lh   x5,4(x6)",        { i_type(4, 6, 1, 5, 0x03) }, 1 }, \
	{ "lw   x5,4(x6)",        { i_type(4, 6, 2, 5, 0x03) }, 1 }, \
	{ "lbu  x5,0(x6)",        { i_type(0, 6, 4, 5, 0x03) }, 1 }, \
	{ "lhu  x5,4(x6)",        { i_type(4, 6, 5, 5, 0x03) }, 1 }, \
	{ "sb   x7,4(x6)",        { s_type(4, 7, 6, 0, 0x23) }, 1 }, \
	{ "sh   x7,4(x6)",        { s_type(4, 7, 6, 1, 0x23) }, 1 }, \
	{ "sw   x7,4(x6)",        { s_type(4, 7, 6, 2, 0x23) }, 1 }, \
	{ "fence",                { 0x0000000f }, 1 }, \
	{ "beq  x6,x7,-4",        { b_type(-4, 7, 6, 0, 0x63) }, 1 }, \
	{ "bne  x6,x7,-4",        { b_type(-4, 7, 6, 1, 0x63) }, 1 }, \
	{ "blt  x6,x7,-4",        { b_type(-4, 7, 6, 4, 0x63) }, 1 }, \
	{ "bgeu x6,x7,-4",        { b_type(-4, 7, 6, 7, 0x63) }, 1 }, \
	{ "jal  x1,-4",           { j_type(-4, 1, 0x6f) }, 1 }, \
	{ "jalr x1,8(x6)",        { i_type(8, 6, 0, 1, 0x67) }, 1 }, \
	{ "ecall (declined after an addi)", \
	  { i_type(1, 6, 0, 5, 0x13), 0x00000073 }, 2 }, \
}

int main(void)
{
	static uint8_t arena[512 * 1024];
	static uint8_t ram[64 * 1024];
	struct piece corpus[] = CORPUS;
	rv32_t s;
	unsigned i;

	memset(&s, 0, sizeof s);
	s.ram = ram;
	s.ram_size = sizeof ram;
	s.reservation = 0xffffffffu;

	if (!rv32_jit_init(arena, sizeof arena)) {
		fprintf(stderr, "the arena is too small\n");
		return 1;
	}

	printf("\t.text\n\t.align 1\n");
	for (i = 0; i < sizeof corpus / sizeof corpus[0]; ++i) {
		const struct piece *p = &corpus[i];
		uint32_t was = rv32_jit.code_used;
		unsigned k;

		memset(ram, 0, sizeof ram);
		for (k = 0; k < p->n; ++k) {
			uint32_t ir = p->ir[k];
			uint8_t *at = ram + 4 * k;

			at[0] = (uint8_t)ir;
			at[1] = (uint8_t)(ir >> 8);
			at[2] = (uint8_t)(ir >> 16);
			at[3] = (uint8_t)(ir >> 24);
		}
		if (!rv32_jit_block(&s, RV_RAM_BASE)) {
			printf("; %s: not translated\n", p->what);
			continue;
		}
		printf("\n; ---- %s  (%u bytes)\n", p->what,
		       (unsigned)(rv32_jit.code_used - was));
		printf("\t.globl ");
		label(p->what);
		printf("\n");
		label(p->what);
		printf(":\n");
		for (; was < rv32_jit.code_used; was += 2)
			printf("\t.short 0x%04x\n",
			       rv32_jit.code[was] | (rv32_jit.code[was + 1] << 8));
		/* Each piece starts over: the map would otherwise find the last
		   one's block at the same guest pc. */
		{
			uint32_t keep = rv32_jit.code_used;

			rv32_jit_flush();
			rv32_jit.code_used = keep;
		}
	}
	return 0;
}
