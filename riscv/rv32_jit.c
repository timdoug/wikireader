/* rv32_jit.c - turn guest instructions into C33 instructions, once.
 *
 * The interpreter's cost is dominated by work that does not depend on the
 * machine at all: README.md measures 40.1 cycles of every 75 inside DISPATCH,
 * taking the instruction word apart and turning it into a table index and two
 * register offsets.  That is the same answer every time the instruction runs.
 * This does it once and keeps the answer.
 *
 * Registers.  The device measured the difference between a translation that
 * keeps guest registers in C33 registers and one that fetches every operand
 * from the register file as 3.5 cycles a guest instruction against 19.9, so
 * this allocates.  Six C33 registers are free once the runtime's are live, and
 * each block gets its six most-used guest registers in them: its *map*, fixed
 * at translation and recorded with the block.  A block has two entries.  The
 * cold one loads the map from x[] and the warm one assumes it is already in
 * place, which is what a loop's back edge and a trace's fall-through use.
 * Registers the block writes are stored back to x[] when it leaves for
 * anywhere that will not carry them, and the invariant that pays for all of
 * this is simple: every register in the map holds its guest register's true
 * value from the warm entry on, so a block can give up anywhere and the
 * runtime writes the six back by the map without the block spending a word.
 *
 * Structure.  A *trace* is translated at a time, not a basic block: the
 * fall-through of a conditional branch and the target of an unconditional jump
 * are laid out immediately after their predecessor and cost nothing at all to
 * reach, which is what `exit_none` measures against `exit_link`'s 23.4 cycles.
 * The successor's map is chosen to keep whatever the predecessor already has
 * in a register, and the fall-through writes back and loads only what changes.
 * A trace stops at an indirect jump, at an instruction this cannot translate,
 * or when it reaches code that already exists.  Each basic block inside it is
 * still registered by its own guest pc, so a jump into the middle finds it.
 *
 * Cold paths.  A load or store checks its address against guest RAM, with
 * one compare that covers both ends and the alignment; a jalr checks its
 * target.  Every check branches *out* to a cold section emitted after the
 * region, so the ordinary path falls straight through and never pays for a
 * taken branch.  The cold section is where the device helpers are called and
 * where a block gives up: three words naming the block, the instruction and
 * the reason, and a jump to the runtime.
 *
 * Nothing watches stores.  Code that changes is announced by the guest's
 * fence.i, as RISC-V requires, and that is when every translation is checked
 * against the words it came from and the changed ones retired.
 *
 * Exits that cannot be laid out inline are three words of `xld.w %r13,pc`
 * followed by a jump to the runtime's lookup.  When the target is translated
 * those three words are overwritten with a direct jump to it -- the same size,
 * so nothing has to move -- and to its warm entry when the maps agree.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "rv32.h"
#include "rv32_jit.h"

rv32_jit_t rv32_jit;
uint32_t rv32_jit_map[RV32_JIT_SLOTS * 2];

/* The offsets only have to hold where the assembly runs; the build machine's
   pointers are twice the size and put the words somewhere else. */
#if __SIZEOF_POINTER__ == 4
_Static_assert(offsetof(rv32_jit_t, map) == RV32_JOFF_MAP, "asm offset");
_Static_assert(offsetof(rv32_jit_t, back) == RV32_JOFF_BACK, "asm offset");
_Static_assert(offsetof(rv32_jit_t, spill) == RV32_JOFF_SPILL, "asm offset");
_Static_assert(offsetof(rv32_jit_t, csrval) == RV32_JOFF_CSRVAL, "asm offset");
#endif

/* The runtime, in rv32_jitrt.s.  The device stubs return into the code cache;
   the other two never do. */
void rv32_jit_stub_fault(void);
void rv32_jit_stub_dev_load(void);
void rv32_jit_stub_dev_store(void);
void rv32_jit_stub_divop(void);
void rv32_jit_stub_csr(void);
void rv32_jit_stub_amo(void);
void rv32_jit_stub_indirect(void);


#define NSLOT RV32_JIT_NSLOT

struct jit_block {
	uint32_t pc;     /* the guest pc it starts at; 0 once retired */
	uint32_t off;    /* its cold entry: where the map is loaded */
	uint32_t warm;   /* its warm entry: the map is already in place */
	uint32_t sum;    /* of the guest words it was translated from */
	uint16_t n;      /* guest instructions in it */
	uint16_t rfirst; /* its region: the first block's index, and how many */
	uint16_t rn;
	uint8_t  map[NSLOT];
};

struct jit_link {
	uint32_t pc;     /* the guest pc this exit wants */
	uint32_t off;    /* the three words to overwrite when it exists */
	uint32_t next;
};

/* ---- the C33 ------------------------------------------------------------
 *
 * Every instruction is one 16-bit word, little endian, in one of three shapes.
 * A register form is (op8 << 8) | (b << 4) | a; an immediate form is
 * (op6 << 10) | (imm6 << 4) | a; a branch is (op8 << 8) | disp8, where the
 * displacement is in halfwords from the instruction itself.
 *
 * An `ext` prefix widens the immediate of whatever follows: one prefix carries
 * 13 bits above the instruction's own field, two carry 26.  Data immediates
 * are sign-extended from the composed width, which is why a value that does
 * not fit six bits gets two prefixes rather than one that would be read as
 * negative.  For a branch the first prefix contributes only its bits 12:3.
 */

enum {
	R0 = 0, R1, R2, R3, R4, R5, R6, R7, R8, R9, R10, R11, R12, R13, R14, R15
};

/* The seven a block may hold guest registers in, in spill order. */
static const uint8_t slot_reg[NSLOT] = { R1, R7, R9, R10, R11, R12, R14 };

enum {                           /* register forms */
	O_MOV   = 0x2e,          /* ld.w  %ra,%rb        */
	O_LDB   = 0x20, O_LDUB = 0x24, O_LDH = 0x28, O_LDUH = 0x2c, O_LDW = 0x30,
	O_STB   = 0x34, O_STH  = 0x38, O_STW = 0x3c,
	O_ADD   = 0x22, O_SUB  = 0x26, O_CMP = 0x2a,
	O_AND   = 0x32, O_OR   = 0x36, O_XOR = 0x3a, O_NOT = 0x3e,
	O_SRL   = 0x89, O_SLL  = 0x8d, O_SRA = 0x91,
	O_MLT   = 0xaa, O_MLTU = 0xae,
	O_SPEC  = 0xa4,          /* ld.w  %ra,%psr (b=0) / %alr (b=2) / %ahr (b=3) */
};

enum {                           /* immediate forms */
	I_ADD = 0x18, I_SUB = 0x19, I_CMP = 0x1a, I_MOV = 0x1b,
	I_AND = 0x1c, I_OR  = 0x1d, I_XOR = 0x1e,
};

/* The shifts do not follow the immediate shape.  Their count is five bits and
   its top bit is not where the sixth bit of an ordinary immediate would be:
   `srl %rd,16` is 0x2300 | ... where `srl %rd,15` is 0x88f0 | ..., and the two
   halves are not one opcode.  Both halves, for each of the three, measured
   against the assembler over every count and register -- `make jitenc`.  The
   first version composed them arithmetically and was right for counts under
   sixteen, which is most of them: Linux booted into a delay loop it could
   never leave, three million instructions later. */
static const uint16_t shift_op[4][2] = {
	{ 0x8800, 0x2300 },      /* srl */
	{ 0x8c00, 0x2700 },      /* sll */
	{ 0x9000, 0x2b00 },      /* sra */
	{ 0x9800, 0x3300 },      /* rr  */
};
enum { S_SRL = 0, S_SLL = 1, S_SRA = 2, S_RR = 3 };

enum {                           /* branches */
	B_GT = 0x08, B_GE = 0x0a, B_LT = 0x0c, B_LE = 0x0e,
	B_UGT = 0x10, B_UGE = 0x12, B_ULT = 0x14, B_ULE = 0x16,
	B_EQ = 0x18, B_NE = 0x1a, B_CALL = 0x1c, B_JP = 0x1e,
};

/* The cold section of a trace: every path that leaves the ordinary one, kept
   until the last block is emitted and then laid out after it.  A device
   access rejoins the block; the others hand the block to the runtime. */
enum { C_FAULT, C_DEV_LOAD, C_DEV_STORE, C_EXIT };

struct cold {
	uint8_t  kind;
	uint8_t  reg;           /* a device load's destination, a store's value */
	uint8_t  align;         /* the access's alignment mask, checked again */
	uint8_t  nsite;
	uint32_t site[3];       /* the prefixed branches that come here */
	uint32_t rejoin;        /* where a device access goes back to */
	uint32_t code;          /* what the runtime is told, or an exit's pc */
};

#define COLD_MAX 4096
static struct cold colds[COLD_MAX];

struct tc {
	rv32_t   *s;
	uint8_t  *base;     /* where the code will run */
	uint32_t  off;
	uint32_t  end;      /* the offset it must stop before */
	int       full;

	/* The block being emitted. */
	uint8_t   map[NSLOT];   /* guest register in each slot, 0 = none */
	uint8_t   hreg[32];     /* host register holding each guest one, 0 = none */
	uint32_t  wb;           /* guest registers x[] is stale for */
	unsigned  bidx;         /* its index, for the runtime */
	unsigned  i;            /* the instruction being emitted */

	unsigned  ncold;
	uint32_t  coldbytes;    /* what the cold section will take */

	unsigned  nrb;          /* the region's blocks, and the jumps between */
	unsigned  nfix;         /* them still to be filled in */
};

/* The emitters are inlined outright.  The translator runs from SDRAM, where
   a call and its return are two fetch restarts, and a guest instruction
   goes through a few dozen of these: it was seven thousand cycles a guest
   instruction with them as calls. */
#define INLINE static inline __attribute__((always_inline))

INLINE void w(struct tc *t, unsigned v)
{
	if (t->off + 2 > t->end) {
		t->full = 1;
		return;
	}
	*(uint16_t *)(t->base + t->off) = (uint16_t)v;     /* always even */
	t->off += 2;
}

INLINE uint32_t run_at(struct tc *t)
{
	return (uint32_t)(uintptr_t)t->base + t->off;
}

INLINE void ext(struct tc *t, uint32_t v)
{
	w(t, 0xc000u | (v & 0x1fffu));
}

INLINE void rr(struct tc *t, unsigned op, unsigned a, unsigned b)
{
	w(t, (op << 8) | (b << 4) | a);
}

/* An immediate wide enough to need prefixes gets both of them whenever it does
   not fit nineteen bits signed, because one prefix composes a 19-bit field the
   core then sign-extends. */
INLINE void ri(struct tc *t, unsigned op, unsigned a, uint32_t v)
{
	int32_t sv = (int32_t)v;

	if (sv < -32 || sv > 31) {
		if (sv < -(1 << 18) || sv > (1 << 18) - 1)
			ext(t, v >> 19);
		ext(t, v >> 6);
	}
	w(t, (op << 10) | ((v & 0x3f) << 4) | a);
}

/* add and sub are the exception: they zero-extend their immediate from the
   composed width where everything else sign-extends, so the assembler spells a
   negative value as the opposite mnemonic and so does this.  Getting it wrong
   is quiet -- `addi gp,gp,-344` came out 0x80000 too high, which is exactly one
   missing sign bit at the 19-bit width one prefix composes. */
INLINE void addimm(struct tc *t, unsigned rd, uint32_t v, int sub)
{
	if ((int32_t)v < 0) {
		sub = !sub;
		v = (uint32_t)0 - v;
	}
	/* Six bits unsigned where the others are six bits signed, which is the
	   other half of the same difference. */
	if (v > 63) {
		if (v > 0x7ffff)
			ext(t, v >> 19);
		ext(t, v >> 6);
	}
	w(t, ((sub ? I_SUB : I_ADD) << 10) | ((v & 0x3f) << 4) | rd);
}

/* A shift count is not sign-extended and is always in range, so it never
   needs a prefix. */
INLINE void shift(struct tc *t, unsigned op, unsigned a, unsigned n)
{
	n &= 0x1f;
	w(t, shift_op[op][n >= 16] | ((n & 0xf) << 4) | a);
}

/* A jump or call to an address outside the reach of the eight-bit field.
   Always three words, which is what lets a link be patched in place. */
INLINE void xjump(struct tc *t, unsigned op, uint32_t target)
{
	uint32_t self = run_at(t) + 4;          /* the branch, past its prefixes */
	int32_t d = (int32_t)(target - self) / 2;
	uint32_t h = (uint32_t)d;

	ext(t, ((h >> 21) & 0x3ff) << 3);
	ext(t, h >> 8);
	w(t, (op << 8) | (h & 0xff));
}

/* A forward branch over a few words of its own instruction: emitted with a
   zero displacement and filled in once its target is known. */
INLINE uint32_t fwd(struct tc *t, unsigned op)
{
	uint32_t at = t->off;

	w(t, op << 8);
	return at;
}

INLINE void land(struct tc *t, uint32_t at)
{
	if (t->full)
		return;
	t->base[at] = (uint8_t)((t->off - at) / 2);
}

/* A conditional branch that can reach the cold section, wherever the trace
   ends: one prefix carries thirteen bits above the eight-bit field, and the
   two together are read as a 21-bit signed displacement. */
INLINE uint32_t jfar(struct tc *t, unsigned op)
{
	uint32_t at = t->off;

	ext(t, 0);
	w(t, op << 8);
	return at;
}

INLINE void land_far(struct tc *t, uint32_t at, uint32_t target)
{
	uint32_t h = (uint32_t)((int32_t)(target - (at + 2)) / 2);

	if (t->full)
		return;
	t->base[at] = (uint8_t)(h >> 8);
	t->base[at + 1] = (uint8_t)(0xc0 | ((h >> 16) & 0x1f));
	t->base[at + 2] = (uint8_t)h;
}

/* ---- the guest register file ------------------------------------------ */

/* Straight to and from x[], for a register the block has no slot for. */
INLINE void getx(struct tc *t, unsigned h, unsigned r)
{
	if (r == 0) {
		ri(t, I_MOV, h, 0);
		return;
	}
	ext(t, r * 4);
	rr(t, O_LDW, h, R0);
}

INLINE void putx(struct tc *t, unsigned r, unsigned h)
{
	if (r == 0)
		return;
	ext(t, r * 4);
	rr(t, O_STW, h, R0);
}

/* A host register holding x[r]: the slot it lives in, or `into` after
   fetching it there. */
INLINE unsigned use(struct tc *t, unsigned r, unsigned into)
{
	if (t->hreg[r])
		return t->hreg[r];
	getx(t, into, r);
	return into;
}

/* Where to compute x[r]: its slot, or the scratch offered. */
INLINE unsigned def(struct tc *t, unsigned r, unsigned scratch)
{
	return t->hreg[r] ? t->hreg[r] : scratch;
}

/* The value for x[r] is in h.  A slot keeps it and owes it to x[] at the
   block's exit; anything else is stored now. */
INLINE void done(struct tc *t, unsigned r, unsigned h)
{
	if (r == 0)
		return;
	if (t->hreg[r]) {
		t->wb |= 1u << r;
		return;
	}
	putx(t, r, h);
}

/* Store every register in `set` from the slot that holds it. */
static void writeback(struct tc *t, uint32_t set)
{
	unsigned k;

	for (k = 0; k < NSLOT; ++k) {
		unsigned g = t->map[k];

		if (g && (set & (1u << g)))
			putx(t, g, slot_reg[k]);
	}
}

/* ---- the cold section -------------------------------------------------- */

INLINE struct cold *cold_new(struct tc *t, unsigned kind, unsigned reg,
			     unsigned why)
{
	struct cold *c = &colds[t->ncold++];

	c->kind = (uint8_t)kind;
	c->reg = (uint8_t)reg;
	c->align = 0;
	c->nsite = 0;
	c->rejoin = 0;
	c->code = t->bidx | (t->i << 16) | (why << 24);
	/* The most each kind emits, in bytes, so the room check can be made
	   before the block and not per word. */
	t->coldbytes += kind == C_DEV_STORE ? 36 : kind == C_DEV_LOAD ? 32 :
			kind == C_EXIT ? 4 * NSLOT + 12 : 12;
	return c;
}

INLINE void cold_site(struct cold *c, uint32_t at)
{
	c->site[c->nsite++] = at;
}

static void region_jump(struct tc *t, uint32_t target);

static void emit_cold(struct tc *t)
{
	uint32_t base = (uint32_t)(uintptr_t)t->base;
	unsigned k, j;

	for (k = 0; k < t->ncold; ++k) {
		struct cold *c = &colds[k];

		uint32_t bad[2];
		unsigned nbad = 0;

		for (j = 0; j < c->nsite; ++j)
			land_far(t, c->site[j], t->off);
		if (c->kind == C_EXIT) {
			region_jump(t, c->code);
			continue;
		}
		/* The one branch a memory access takes covers misaligned as
		   well as outside RAM.  Only a device access goes on from here,
		   so the two are told apart now, where it costs nothing. */
		if (c->kind != C_FAULT && c->align) {
			rr(t, O_MOV, R13, R4);
			ri(t, I_AND, R13, c->align);
			bad[nbad++] = fwd(t, B_NE);
		}
		switch (c->kind) {
		case C_DEV_LOAD:
			xjump(t, B_CALL,
			      (uint32_t)(uintptr_t)rv32_jit_stub_dev_load);
			if (c->reg != R5)
				rr(t, O_MOV, c->reg, R5);
			xjump(t, B_JP, base + c->rejoin);
			break;
		case C_DEV_STORE:
			if (c->reg != R5)
				rr(t, O_MOV, R5, c->reg);
			xjump(t, B_CALL,
			      (uint32_t)(uintptr_t)rv32_jit_stub_dev_store);
			bad[nbad++] = fwd(t, B_NE);     /* C has to do it */
			xjump(t, B_JP, base + c->rejoin);
			break;
		default:
			break;
		}
		if (c->kind == C_FAULT || nbad) {
			for (j = 0; j < nbad; ++j)
				land(t, bad[j]);
			ri(t, I_MOV, R5, c->code);
			xjump(t, B_JP, (uint32_t)(uintptr_t)rv32_jit_stub_fault);
		}
	}
	t->ncold = 0;
	t->coldbytes = 0;
}

/* ---- guest instructions ------------------------------------------------ */

/* Guest RAM is word-aligned and so is every pc that gets here, and the C33
   is little-endian like the guest: one load. */
INLINE uint32_t fetch(rv32_t *s, uint32_t pc)
{
	return *(const uint32_t *)(s->ram + (pc - RV_RAM_BASE));
}

INLINE int in_ram(rv32_t *s, uint32_t pc)
{
	return pc >= RV_RAM_BASE && pc - RV_RAM_BASE < s->ram_size;
}

INLINE int good_target(rv32_t *s, uint32_t pc)
{
	return !(pc & 3) && in_ram(s, pc);
}

INLINE int32_t imm_i(uint32_t ir)  { return (int32_t)ir >> 20; }
INLINE int32_t imm_s(uint32_t ir)
{
	return (int32_t)((ir & 0xfe000000u)) >> 20 | (int32_t)((ir >> 7) & 0x1f);
}
INLINE int32_t imm_b(uint32_t ir)
{
	return (int32_t)((ir & 0x80000000u)) >> 19 |
	       (int32_t)((ir >> 7) & 0x1e) |
	       (int32_t)((ir >> 20) & 0x7e0) |
	       (int32_t)((ir << 4) & 0x800);
}
INLINE int32_t imm_j(uint32_t ir)
{
	return (int32_t)((ir & 0x80000000u)) >> 11 |
	       (int32_t)(ir & 0xff000) |
	       (int32_t)((ir >> 9) & 0x800) |
	       (int32_t)((ir >> 20) & 0x7fe);
}

/* Can this be translated at all?  Everything rare stays in the C interpreter,
   for the same reason the assembly hot path leaves it there: it is written
   once, where it can be read.

   JIT_SKIP is a bisecting switch: each bit takes one class of guest
   instruction away from the translator and gives it to the C interpreter,
   which is known good.  It exists because a translation that is wrong on one
   opcode boots Linux into the weeds a million instructions later, and this
   says which opcode in one run apiece. */
#ifndef JIT_SKIP
#define JIT_SKIP 0
#endif

static int can_do(uint32_t ir)
{
	unsigned op = ir & 0x7f, f3 = (ir >> 12) & 7, f7 = ir >> 25;

	switch (op) {
	case 0x37: case 0x17:                       /* lui, auipc */
		return !(JIT_SKIP & 128);
	case 0x6f:                                  /* jal */
		return !(JIT_SKIP & 32);
	case 0x67:                                  /* jalr */
		return f3 == 0 && !(JIT_SKIP & 64);
	case 0x63:                                  /* branch */
		return f3 != 2 && f3 != 3 && !(JIT_SKIP & 16);
	case 0x03:                                  /* load */
		return (f3 <= 2 || f3 == 4 || f3 == 5) && !(JIT_SKIP & 1);
	case 0x23:                                  /* store */
		return f3 <= 2 && !(JIT_SKIP & 2);
	case 0x13:                                  /* op-imm */
		if (JIT_SKIP & 4)
			return 0;
		if ((JIT_SKIP & 1024) && f3 == 0)
			return 0;
		if ((JIT_SKIP & 2048) && (f3 == 1 || f3 == 5))
			return 0;
		if ((JIT_SKIP & 4096) && (f3 == 2 || f3 == 3))
			return 0;
		if ((JIT_SKIP & 8192) && (f3 == 4 || f3 == 6 || f3 == 7))
			return 0;
		if (f3 == 1)
			return (ir >> 25) == 0;
		if (f3 == 5)
			return f7 == 0 || f7 == 0x20;
		return 1;
	case 0x33:                                  /* op */
		if (f7 == 1)
			return !(JIT_SKIP & 512);
		if (JIT_SKIP & 8)
			return 0;
		if (f7 == 0)
			return 1;
		return f7 == 0x20 && (f3 == 0 || f3 == 5);
	case 0x0f:                                  /* fence */
		return (f3 == 0 || f3 == 1) && !(JIT_SKIP & 256);
	case 0x73:                                  /* csr: not ecall, mret, wfi */
		return (f3 & 3) != 0 && !(JIT_SKIP & 16384);
	case 0x2f:                                  /* amo, lr, sc */
		if (f3 != 2 || (JIT_SKIP & 32768))
			return 0;
		switch (ir >> 27) {
		case 0x00: case 0x01: case 0x02: case 0x03: case 0x04:
		case 0x08: case 0x0c: case 0x10: case 0x14: case 0x18: case 0x1c:
			return 1;
		default:
			return 0;
		}
	default:
		return 0;
	}
}

/* Which guest registers an instruction reads and writes; 0 for none. */
static void regs_of(uint32_t ir, unsigned *rs1, unsigned *rs2, unsigned *rd)
{
	*rs1 = *rs2 = *rd = 0;
	switch (ir & 0x7f) {
	case 0x37: case 0x17: case 0x6f:
		*rd = (ir >> 7) & 31;
		break;
	case 0x67: case 0x03: case 0x13:
		*rs1 = (ir >> 15) & 31;
		*rd = (ir >> 7) & 31;
		break;
	case 0x63: case 0x23:
		*rs1 = (ir >> 15) & 31;
		*rs2 = (ir >> 20) & 31;
		break;
	case 0x33: case 0x2f:
		*rs1 = (ir >> 15) & 31;
		*rs2 = (ir >> 20) & 31;
		*rd = (ir >> 7) & 31;
		break;
	case 0x73:
		if (!(ir & (4 << 12)))
			*rs1 = (ir >> 15) & 31;
		*rd = (ir >> 7) & 31;
		break;
	default:
		break;
	}
}

/* The host address of a guest access into %r4, checked.

   One compare and one branch cover both ends of RAM and the alignment.  The
   offset is rotated right by the alignment's width: an aligned offset comes
   out divided, and a misaligned one lands its low bits at the top where they
   make it larger than any size.  So `offset rotr 2 < size / 4` says a word
   access is inside RAM and aligned, and %r3 holds size / 4 for exactly this.
   The device path sorts misaligned from device afterwards, out of line, where
   it costs nothing.  This was four words of alignment test and two compares
   with a prefixed branch apiece, and every one of those words is fetched from
   SDRAM every time a loop goes round. */
INLINE struct cold *address(struct tc *t, unsigned rs1, int32_t off,
			    unsigned align, unsigned kind, unsigned reg)
{
	struct cold *c;
	unsigned h1 = use(t, rs1, R4);

	if (h1 != R4)
		rr(t, O_MOV, R4, h1);
	if (off)
		addimm(t, R4, (uint32_t)off, 0);
	rr(t, O_ADD, R4, R8);
	rr(t, O_MOV, R13, R4);
	rr(t, O_SUB, R13, R2);
	c = cold_new(t, kind, reg, RV32_JIT_DECLINE);
	c->align = (uint8_t)align;
	switch (align) {
	case 3:
		shift(t, S_RR, R13, 2);
		rr(t, O_CMP, R13, R3);
		break;
	case 1:
		shift(t, S_RR, R13, 1);
		ri(t, I_CMP, R13, t->s->ram_size >> 1);
		break;
	default:
		shift(t, S_SRL, R13, 2);
		rr(t, O_CMP, R13, R3);
		break;
	}
	cold_site(c, jfar(t, B_UGE));
	return c;
}

/* hd = the comparison just made, as a word.  Unsigned is the carry bit of the
   PSR read straight out; signed is N xor V, and a branch over a load is
   shorter than the arithmetic. */
INLINE void setless(struct tc *t, unsigned hd, int unsigned_)
{
	uint32_t over;

	if (unsigned_) {
		rr(t, O_SPEC, hd, 0);           /* ld.w hd,%psr */
		shift(t, S_SRL, hd, 3);
		ri(t, I_AND, hd, 1);
		return;
	}
	ri(t, I_MOV, hd, 0);
	over = fwd(t, B_GE);
	ri(t, I_MOV, hd, 1);
	land(t, over);
}

/* x[rd] = x[rs1] op x[rs2] on a two-address machine: the destination has to
   start out holding the first operand. */
INLINE void op3(struct tc *t, unsigned op, int commutes,
		unsigned rd, unsigned rs1, unsigned rs2)
{
	unsigned hd = def(t, rd, R4);
	unsigned h1 = use(t, rs1, R4);
	unsigned h2 = rs2 == rs1 ? h1 : use(t, rs2, R5);

	if (hd == h1) {
		rr(t, op, hd, h2);
	} else if (hd == h2) {
		if (commutes) {
			rr(t, op, hd, h1);
		} else {
			if (h1 != R4)
				rr(t, O_MOV, R4, h1);
			rr(t, op, R4, h2);
			rr(t, O_MOV, hd, R4);
		}
	} else {
		rr(t, O_MOV, hd, h1);
		rr(t, op, hd, h2);
	}
	done(t, rd, hd);
}

/* The divides, which have no C33 instruction behind them: the runtime's
   wrapper keeps what the block has live and calls rv32.c. */
static void mcall(struct tc *t, unsigned rd, unsigned rs1, unsigned rs2,
		  unsigned f3)
{
	unsigned hd = def(t, rd, R4);
	unsigned h1 = use(t, rs1, R4);
	unsigned h2 = use(t, rs2, R5);

	if (h1 != R4)
		rr(t, O_MOV, R4, h1);
	if (h2 != R5)
		rr(t, O_MOV, R5, h2);
	ri(t, I_MOV, R13, f3);
	xjump(t, B_CALL, (uint32_t)(uintptr_t)rv32_jit_stub_divop);
	if (hd != R4)
		rr(t, O_MOV, hd, R4);
	done(t, rd, hd);
}

/* mulhsu: the high word of the unsigned product, less the multiplicand when
   it was negative.  The multiply leaves the flags alone, so the sign test can
   be made before it and used after. */
static void mulhsu(struct tc *t, unsigned rd, unsigned rs1, unsigned rs2)
{
	unsigned hd = def(t, rd, R4);
	unsigned h1 = use(t, rs1, R4);
	unsigned h2 = rs2 == rs1 ? h1 : use(t, rs2, R5);
	uint32_t over;

	ri(t, I_CMP, h1, 0);
	rr(t, O_MLTU, h1, h2);
	rr(t, O_SPEC, R4, 3);                   /* ld.w %r4,%ahr */
	over = fwd(t, B_GE);
	rr(t, O_SUB, R4, h2);
	land(t, over);
	if (hd != R4)
		rr(t, O_MOV, hd, R4);
	done(t, rd, hd);
}

/* One guest instruction.  Control transfers emit everything except how the
   block is left, which the trace builder decides. */
static void insn(struct tc *t, uint32_t pc, uint32_t ir)
{
	unsigned op = ir & 0x7f;
	unsigned rd = (ir >> 7) & 31, rs1 = (ir >> 15) & 31, rs2 = (ir >> 20) & 31;
	unsigned f3 = (ir >> 12) & 7, f7 = ir >> 25;
	unsigned hd, h1, h2;

	switch (op) {
	case 0x37:                                      /* lui */
		if (rd) {
			hd = def(t, rd, R4);
			ri(t, I_MOV, hd, ir & 0xfffff000u);
			done(t, rd, hd);
		}
		return;

	case 0x17:                                      /* auipc */
		if (rd) {
			hd = def(t, rd, R4);
			ri(t, I_MOV, hd, pc + (ir & 0xfffff000u));
			done(t, rd, hd);
		}
		return;

	case 0x6f:                                      /* jal */
		if (rd) {
			hd = def(t, rd, R4);
			ri(t, I_MOV, hd, pc + 4);
			done(t, rd, hd);
		}
		return;                                 /* the trace does the jump */

	case 0x67: {                                    /* jalr */
		struct cold *c = cold_new(t, C_FAULT, 0, RV32_JIT_DECLINE);

		h1 = use(t, rs1, R13);
		if (h1 != R13)
			rr(t, O_MOV, R13, h1);
		if (imm_i(ir))
			addimm(t, R13, (uint32_t)imm_i(ir), 0);
		ri(t, I_AND, R13, 0xfffffffeu);         /* bit 0 is cleared, not a fault */
		/* The target has to land inside RAM on a word boundary; anything
		   else is a trap, and traps are built in one place.  The same
		   one compare as a word access: the offset into RAM rotated by
		   two is below size / 4 only if both hold. */
		rr(t, O_MOV, R5, R13);
		rr(t, O_ADD, R5, R8);
		rr(t, O_SUB, R5, R2);
		shift(t, S_RR, R5, 2);
		rr(t, O_CMP, R5, R3);
		cold_site(c, jfar(t, B_UGE));
		if (rd) {
			hd = def(t, rd, R4);
			ri(t, I_MOV, hd, pc + 4);
			done(t, rd, hd);
		}
		return;                                 /* the trace does the jump */
	}

	case 0x63:                                      /* branch */
		h1 = use(t, rs1, R4);
		h2 = rs2 == rs1 ? h1 : use(t, rs2, R5);
		rr(t, O_CMP, h1, h2);
		return;                                 /* the trace does the jump */

	case 0x03: {                                    /* load */
		static const unsigned kind[8] = {
			O_LDB, O_LDH, O_LDW, 0, O_LDUB, O_LDUH, 0, 0
		};
		struct cold *c;

		/* A load into x0 still happens: a device read has effects. */
		hd = rd ? def(t, rd, R5) : R5;
		c = address(t, rs1, imm_i(ir),
			    f3 == 2 ? 3 : (f3 == 1 || f3 == 5) ? 1 : 0,
			    C_DEV_LOAD, hd);
		rr(t, kind[f3], hd, R4);
		c->rejoin = t->off;     /* the device leaves its word there too */
		done(t, rd, hd);
		return;
	}

	case 0x23: {                                    /* store */
		static const unsigned kind[4] = { O_STB, O_STH, O_STW, 0 };
		struct cold *c;
		unsigned hv = use(t, rs2, R5);

		/* Nothing watches where it goes: a store into code is not
		   seen until the guest's fence.i, which is what RISC-V says,
		   and a store into the reserved word does not break the
		   reservation, which no lr/sc loop relies on. */
		c = address(t, rs1, imm_s(ir), f3 == 2 ? 3 : f3 == 1 ? 1 : 0,
			    C_DEV_STORE, hv);
		rr(t, kind[f3], hv, R4);        /* the value first, the base second */
		c->rejoin = t->off;     /* a device store is already done */
		return;
	}

	case 0x13:                                      /* op-imm */
		if (!rd)
			return;
		hd = def(t, rd, R4);
		if (f3 == 0 && rs1 == 0) {              /* li */
			ri(t, I_MOV, hd, (uint32_t)imm_i(ir));
			done(t, rd, hd);
			return;
		}
		if (f3 == 2 || f3 == 3) {               /* slti, sltiu */
			h1 = use(t, rs1, R4);
			ri(t, I_CMP, h1, (uint32_t)imm_i(ir));
			setless(t, hd, f3 == 3);
			done(t, rd, hd);
			return;
		}
		h1 = use(t, rs1, hd);
		if (h1 != hd)
			rr(t, O_MOV, hd, h1);
		switch (f3) {
		case 0: if (imm_i(ir)) addimm(t, hd, (uint32_t)imm_i(ir), 0); break;
		case 1: shift(t, S_SLL, hd, rs2); break;
		case 4: ri(t, I_XOR, hd, (uint32_t)imm_i(ir)); break;
		case 5: shift(t, f7 == 0x20 ? S_SRA : S_SRL, hd, rs2); break;
		case 6: ri(t, I_OR,  hd, (uint32_t)imm_i(ir)); break;
		default: ri(t, I_AND, hd, (uint32_t)imm_i(ir)); break;
		}
		done(t, rd, hd);
		return;

	case 0x33:                                      /* op */
		if (!rd)
			return;
		if (f7 == 1) {
			if (f3 == 2) {
				mulhsu(t, rd, rs1, rs2);
				return;
			}
			if (f3 >= 4) {
				mcall(t, rd, rs1, rs2, f3);     /* the divides */
				return;
			}
			h1 = use(t, rs1, R4);
			h2 = rs2 == rs1 ? h1 : use(t, rs2, R5);
			rr(t, f3 == 1 ? O_MLT : O_MLTU, h1, h2);
			hd = def(t, rd, R4);
			rr(t, O_SPEC, hd, f3 == 0 ? 2 : 3);     /* %alr or %ahr */
			done(t, rd, hd);
			return;
		}
		switch (f3) {
		case 0: op3(t, f7 == 0x20 ? O_SUB : O_ADD, f7 != 0x20, rd, rs1, rs2); return;
		case 1: op3(t, O_SLL, 0, rd, rs1, rs2); return;     /* the core keeps 5 bits of the count */
		case 2: case 3:
			h1 = use(t, rs1, R4);
			h2 = rs2 == rs1 ? h1 : use(t, rs2, R5);
			rr(t, O_CMP, h1, h2);
			hd = def(t, rd, R4);
			setless(t, hd, f3 == 3);
			done(t, rd, hd);
			return;
		case 4: op3(t, O_XOR, 1, rd, rs1, rs2); return;
		case 5: op3(t, f7 == 0x20 ? O_SRA : O_SRL, 0, rd, rs1, rs2); return;
		case 6: op3(t, O_OR, 1, rd, rs1, rs2); return;
		default: op3(t, O_AND, 1, rd, rs1, rs2); return;
		}

	case 0x73: {                                    /* csrrw/s/c, and -i */
		struct cold *c = cold_new(t, C_FAULT, 0, RV32_JIT_DECLINE);

		if (f3 & 4)
			ri(t, I_MOV, R4, rs1);          /* the immediate */
		else {
			h1 = use(t, rs1, R4);
			if (h1 != R4)
				rr(t, O_MOV, R4, h1);
		}
		ri(t, I_MOV, R13, ir);
		xjump(t, B_CALL, (uint32_t)(uintptr_t)rv32_jit_stub_csr);
		cold_site(c, jfar(t, B_NE));            /* the helper declined */
		if (rd) {
			hd = def(t, rd, R5);
			if (hd != R5)
				rr(t, O_MOV, hd, R5);
			done(t, rd, hd);
		}
		return;
	}

	case 0x2f: {                                    /* amo, lr, sc */
		unsigned hv = use(t, rs2, R5);

		/* The address is checked like a store's; a device address is
		   a decline, since there are no atomics on a device. */
		address(t, rs1, 0, 3, C_FAULT, 0);
		if (hv != R5)
			rr(t, O_MOV, R5, hv);
		ri(t, I_MOV, R13, ir);
		xjump(t, B_CALL, (uint32_t)(uintptr_t)rv32_jit_stub_amo);
		if (rd) {
			hd = def(t, rd, R5);
			if (hd != R5)
				rr(t, O_MOV, hd, R5);
			done(t, rd, hd);
		}
		return;
	}

	default:                                        /* fence: nothing is
							   cached or reordered */
		return;
	}
}

/* ---- blocks, traces and the cache -------------------------------------- */

/* A block ends at the first control transfer, at the first instruction this
   cannot translate, or at the cap -- whichever comes first.  A jump whose
   target is outside RAM or off a word is the C interpreter's, which raises
   the trap.  Nothing is emitted here; the count is what the prologue charges
   the batch for. */
enum { T_BRANCH, T_JAL, T_JALR, T_DECLINE, T_CAP };

#define BLOCK_CAP 200
#define INSN_ROOM 128       /* the most any one instruction emits, hot and cold */
#define BLOCK_ROOM 256      /* cold entry, budget check and exits */

static unsigned scan(rv32_t *s, uint32_t pc, int *term)
{
	unsigned n;

	for (n = 0; n < BLOCK_CAP; ++n) {
		uint32_t at = pc + 4 * n, ir;

		if (!in_ram(s, at) || !in_ram(s, at + 3)) {
			*term = T_DECLINE;
			return n;
		}
		ir = fetch(s, at);
		if (!can_do(ir)) {
			*term = T_DECLINE;
			return n;
		}
		switch (ir & 0x7f) {
		case 0x63:
			if (!good_target(s, at + (uint32_t)imm_b(ir))) {
				*term = T_DECLINE;
				return n;
			}
			*term = T_BRANCH;
			return n + 1;
		case 0x6f:
			if (!good_target(s, at + (uint32_t)imm_j(ir))) {
				*term = T_DECLINE;
				return n;
			}
			*term = T_JAL;
			return n + 1;
		case 0x67:
			*term = T_JALR;
			return n + 1;
		default:
			break;
		}
	}
	*term = T_CAP;
	return n;
}

static uint32_t *slot(uint32_t pc)
{
	return rv32_jit.map + 2 * ((pc >> 2) & RV32_JIT_MASK);
}

static uint32_t bhash(uint32_t pc)
{
	return (pc >> 2) * 2654435761u;
}

/* Open addressing with a tombstone: a retired block leaves H_DEAD behind so
   that probing continues past it, and a new block may take the place. */
enum { H_FREE = 0xffff, H_DEAD = 0xfffe };

static struct jit_block *find(uint32_t pc)
{
	uint32_t m = rv32_jit.blk_hash_mask;
	uint32_t i = (bhash(pc) >> 8) & m;

	for (;;) {
		uint16_t v = rv32_jit.blk_hash[i];

		if (v == H_FREE)
			return NULL;
		if (v != H_DEAD && rv32_jit.blk[v].pc == pc)
			return &rv32_jit.blk[v];
		i = (i + 1) & m;
	}
}

static uint32_t entry_of(struct jit_block *b)
{
	return (uint32_t)(uintptr_t)rv32_jit.code + b->off;
}

static void patch(uint32_t off, uint32_t code)
{
	struct tc t;

	memset(&t, 0, sizeof t);
	t.base = rv32_jit.code;
	t.off = off;
	t.end = off + 6;
	xjump(&t, B_JP, code);
}

/* Every exit whose target was not laid out next waits on this list until the
   target exists, and is then overwritten with a direct jump to it. */
static void link_add(uint32_t pc, uint32_t off)
{
	uint32_t m = rv32_jit.link_hash_mask;
	uint32_t i = (bhash(pc) >> 8) & m;
	struct jit_link *l;

	if (rv32_jit.link_used >= rv32_jit.link_max)
		return;                 /* it stays indirect, which is only slower */
	l = &rv32_jit.link[rv32_jit.link_used];
	l->pc = pc;
	l->off = off;
	l->next = rv32_jit.link_hash[i] == 0xffff ? 0xffffffffu :
		  rv32_jit.link_hash[i];
	rv32_jit.link_hash[i] = (uint16_t)rv32_jit.link_used;
	rv32_jit.link_used++;
}

static void link_resolve(struct jit_block *b)
{
	uint32_t m = rv32_jit.link_hash_mask;
	uint32_t i = (bhash(b->pc) >> 8) & m;
	uint32_t at = rv32_jit.link_hash[i] == 0xffff ? 0xffffffffu :
		      rv32_jit.link_hash[i];

	while (at != 0xffffffffu) {
		struct jit_link *l = &rv32_jit.link[at];

		if (l->pc == b->pc && l->off != 0xffffffffu) {
			patch(l->off, entry_of(b));
			l->off = 0xffffffffu;
		}
		at = l->next;
	}
}

/* What a block was translated from, for fence.i to check against. */
static uint32_t checksum(rv32_t *s, uint32_t pc, unsigned n)
{
	uint32_t sum = 0;
	unsigned i;

	for (i = 0; i < n; ++i)
		sum = sum * 31 + fetch(s, pc + 4 * i);
	return sum;
}

/* The caller has checked there is a descriptor free. */
static struct jit_block *record(rv32_t *s, uint32_t pc, uint32_t off,
				uint32_t warm, unsigned n, const uint8_t *map,
				unsigned rfirst, unsigned rn)
{
	struct jit_block *b = &rv32_jit.blk[rv32_jit.blk_used];
	uint32_t m, i;

	b->pc = pc;
	b->off = off;
	b->warm = warm;
	b->sum = checksum(s, pc, n);
	b->n = (uint16_t)n;
	b->rfirst = (uint16_t)rfirst;
	b->rn = (uint16_t)rn;
	memcpy(b->map, map, NSLOT);

	m = rv32_jit.blk_hash_mask;
	i = (bhash(pc) >> 8) & m;
	while (rv32_jit.blk_hash[i] != H_FREE && rv32_jit.blk_hash[i] != H_DEAD)
		i = (i + 1) & m;
	rv32_jit.blk_hash[i] = (uint16_t)rv32_jit.blk_used;
	rv32_jit.blk_used++;

	slot(pc)[0] = pc;
	slot(pc)[1] = (uint32_t)(uintptr_t)(rv32_jit.code + off);

	link_resolve(b);
	rv32_jit.blocks++;
	return b;
}

/* Three words naming a guest pc, then the jump to the runtime's lookup.  The
   three words are what a patch overwrites, so they are emitted long-hand
   rather than through ri(), which would shorten a small value. */
static uint32_t exit_to(struct tc *t, uint32_t pc)
{
	uint32_t site = t->off;

	ext(t, pc >> 19);
	ext(t, pc >> 6);
	w(t, (I_MOV << 10) | ((pc & 0x3f) << 4) | R13);
	xjump(t, B_JP, (uint32_t)(uintptr_t)rv32_jit_stub_indirect);
	return site;
}

/* ---- regions ------------------------------------------------------------
 *
 * The unit of allocation.  A region is every block reachable from its entry
 * through branches and plain jumps -- not calls, which are where a function
 * ends and another's registers begin -- up to a cap.  All of it shares one
 * map, chosen by use with the blocks inside loops counted several times over,
 * so a jump inside the region loads and stores nothing at all.  Leaving the
 * region writes back what it wrote; entering it loads the map.
 *
 * Blocks are found depth first with the fall-through taken before the branch
 * target, which lays each one out after its predecessor: the trace shape that
 * costs nothing to follow.  The cold entries -- the loads, and a jump to the
 * block's inline code -- are gathered at the front of the region, so that a
 * fall-through lands on the budget check and not on a load.
 */

#define REGION_BLOCKS 64
#define REGION_INSNS  400

struct rblock {
	uint32_t pc;
	uint32_t off;           /* cold entry */
	uint32_t warm;          /* the inline code */
	uint16_t n;
	uint8_t  term;
	uint8_t  weight;        /* how many loops it sits inside, plus one */
	uint8_t  head;          /* a backward jump lands here */
	uint8_t  unroll;        /* copies of the body laid out */
};

enum { FIX_JUMP, FIX_BRANCH };

struct fixup {
	uint32_t site;          /* an xjump, or a prefixed branch, to fill in */
	uint8_t  target;        /* ...with this block's warm entry */
	uint8_t  kind;
};

static struct rblock rb[REGION_BLOCKS];
static struct fixup fixups[REGION_BLOCKS * 8];

static int in_region(unsigned nrb, uint32_t pc)
{
	unsigned i;

	for (i = 0; i < nrb; ++i)
		if (rb[i].pc == pc)
			return (int)i;
	return -1;
}

static uint32_t succ_taken(rv32_t *s, const struct rblock *r)
{
	uint32_t last = r->pc + 4 * (r->n - 1);
	uint32_t ir = fetch(s, last);

	return r->term == T_BRANCH ? last + (uint32_t)imm_b(ir) :
	       r->term == T_JAL ? last + (uint32_t)imm_j(ir) : 0;
}

/* Every block from pc, depth first, fall-through first.  A jal that links is
   a call and ends the region; one that does not is a jump and is followed. */
static unsigned discover(rv32_t *s, uint32_t pc)
{
	uint32_t stack[REGION_BLOCKS * 2 + 2];
	unsigned sp = 0, nrb = 0, total = 0, j;

	stack[sp++] = pc;
	while (sp && nrb < REGION_BLOCKS) {
		struct rblock *r;
		int term;
		unsigned n;

		pc = stack[--sp];
		if (in_region(nrb, pc) >= 0 || find(pc))
			continue;
		/* Inside a block the region already has: split that block
		   there rather than lay its tail out twice.  The tail keeps
		   the ending and goes in right after, so the head still falls
		   through into it. */
		for (j = 0; j < nrb; ++j)
			if (rb[j].pc < pc && pc < rb[j].pc + 4 * rb[j].n)
				break;
		if (j < nrb) {
			unsigned k = (pc - rb[j].pc) / 4;

			if (nrb >= REGION_BLOCKS)
				continue;
			memmove(&rb[j + 2], &rb[j + 1], (nrb - j - 1) * sizeof rb[0]);
			nrb++;
			rb[j + 1] = rb[j];
			rb[j + 1].pc = pc;
			rb[j + 1].n = (uint16_t)(rb[j].n - k);
			rb[j].n = (uint16_t)k;
			rb[j].term = T_CAP;
			rb[j].unroll = 1;
			/* The tail may be a loop on its own now -- a function's
			   prologue falling into its loop is exactly this shape. */
			r = &rb[j + 1];
			r->unroll = r->term == T_BRANCH && succ_taken(s, r) == pc &&
				    r->n <= 16 ? (r->n <= 6 ? 4 : 2) : 1;
			continue;
		}
		n = scan(s, pc, &term);
		if (n == 0)
			continue;
		/* A block that runs into a head the region already has stops
		   short of it, so that no code is laid out twice -- unless it
		   is a loop onto its own start, which is worth having whole:
		   the interpreter stops wherever its chunk runs out, so the
		   region is as likely as not entered part way round a loop,
		   and the loop laid out from its head is what gets unrolled. */
		{
			uint32_t last = pc + 4 * (n - 1);
			int self = term == T_BRANCH &&
				   last + (uint32_t)imm_b(fetch(s, last)) == pc;

			if (!self)
				for (j = 0; j < nrb; ++j)
					if (rb[j].pc > pc && rb[j].pc < pc + 4 * n) {
						n = (rb[j].pc - pc) / 4;
						term = T_CAP;
					}
		}
		if (total + n > REGION_INSNS)
			continue;
		r = &rb[nrb++];
		r->pc = pc;
		r->n = (uint16_t)n;
		r->term = (uint8_t)term;
		r->weight = 1;
		r->head = 0;
		/* A loop that is one block long is laid out several times over;
		   see emit_block(). */
		r->unroll = term == T_BRANCH && succ_taken(s, r) == pc && n <= 16
			    ? (n <= 6 ? 4 : 2) : 1;
		total += n;

		if (sp + 2 > sizeof stack / sizeof stack[0])
			break;
		switch (term) {
		case T_BRANCH:
			stack[sp++] = succ_taken(s, r);
			stack[sp++] = pc + 4 * n;
			break;
		case T_JAL:
			if (((fetch(s, pc + 4 * (n - 1)) >> 7) & 31) == 0) {
				stack[sp++] = succ_taken(s, r);
			} else {
				/* A call.  The callee is its own region, but
				   the return lands here, and a return that
				   finds no block interprets a chunk. */
				stack[sp++] = pc + 4 * n;
			}
			break;
		case T_CAP:
			stack[sp++] = pc + 4 * n;
			break;
		default:
			break;
		}
	}
	return nrb;
}

/* The region's map: guest registers by weighted use, with a register worth a
   slot only if it earns back the load and the store the slot costs.  Blocks
   inside a loop -- between a backward jump and its target -- count four times,
   because that is where the time goes. */
static void choose(struct tc *t, unsigned nrb)
{
	unsigned cnt[32];
	unsigned i, j, k, g;

	for (i = 0; i < nrb; ++i) {
		uint32_t target = succ_taken(t->s, &rb[i]);
		int h;

		if (!target || target > rb[i].pc + 4 * (rb[i].n - 1))
			continue;
		/* A backward jump: its target is where the batch is checked,
		   because every cycle in the region goes through one. */
		h = in_region(nrb, target);
		if (h >= 0)
			rb[h].head = 1;
		for (j = 0; j < nrb; ++j)
			if (rb[j].pc >= target && rb[j].pc <= rb[i].pc &&
			    rb[j].weight < 64)
				rb[j].weight += 3;
	}
	memset(cnt, 0, sizeof cnt);
	t->wb = 0;
	for (i = 0; i < nrb; ++i)
		for (j = 0; j < rb[i].n; ++j) {
			unsigned rs1, rs2, rd;

			regs_of(fetch(t->s, rb[i].pc + 4 * j), &rs1, &rs2, &rd);
			cnt[rs1] += rb[i].weight;
			cnt[rs2] += rb[i].weight;
			cnt[rd] += rb[i].weight;
			if (rd)
				t->wb |= 1u << rd;
		}
	memset(t->hreg, 0, sizeof t->hreg);
	memset(t->map, 0, sizeof t->map);
	/* The NSLOT most used, in one pass: each register is slid into a
	   list kept in descending order.  This loop was the translator's
	   single hottest line as a select-the-max repeated NSLOT times. */
	for (g = 1; g < 32; ++g) {
		unsigned c = cnt[g];

		if (c < 2)
			continue;
		for (k = NSLOT; k > 0 && (!t->map[k - 1] || cnt[t->map[k - 1]] < c); --k)
			if (k < NSLOT)
				t->map[k] = t->map[k - 1];
		if (k < NSLOT)
			t->map[k] = (uint8_t)g;
	}
	for (k = 0; k < NSLOT; ++k)
		if (t->map[k])
			t->hreg[t->map[k]] = slot_reg[k];
	/* Only what is both written and held is owed to x[] on the way out. */
	for (g = 1; g < 32; ++g)
		if (!t->hreg[g])
			t->wb &= ~(1u << g);
}

/* The C33 branch for a guest condition, and its inverse for when the taken
   path is laid out inline and the branch has to jump over it. */
static unsigned cond_of(unsigned f3, int inverted)
{
	switch (f3 ^ (inverted ? 1 : 0)) {
	case 0: return B_EQ;            /* beq  */
	case 1: return B_NE;            /* bne  */
	case 4: return B_LT;            /* blt  */
	case 5: return B_GE;            /* bge  */
	case 6: return B_ULT;           /* bltu */
	default: return B_UGE;          /* bgeu */
	}
}

static void fix(unsigned *nfix, uint32_t site, unsigned target, unsigned kind)
{
	fixups[*nfix].site = site;
	fixups[*nfix].target = (uint8_t)target;
	fixups[*nfix].kind = (uint8_t)kind;
	++*nfix;
}

/* The batch check: stop if it is spent.  Nothing has been charged for the
   block that follows, so a stop here gives nothing back. */
static void budget_check(struct tc *t)
{
	struct cold *c;

	t->i = 0;
	c = cold_new(t, C_FAULT, 0, RV32_JIT_BUDGET);
	ri(t, I_CMP, R6, 0);
	cold_site(c, jfar(t, B_LE));
}

/* Leave the block for a guest pc.  Inside the region it is a jump to the warm
   entry and nothing else.  Outside, everything the region wrote is stored
   first, and then it is a jump to the block if it exists and a link waiting
   for it if not. */
static void region_jump(struct tc *t, uint32_t target)
{
	int j = in_region(t->nrb, target);
	struct jit_block *b;

	if (j >= 0) {
		if (rb[j].warm != 0xffffffffu) {
			xjump(t, B_JP,
			      (uint32_t)(uintptr_t)t->base + rb[j].warm);
		} else {
			fix(&t->nfix, t->off, (unsigned)j, FIX_JUMP);
			xjump(t, B_JP, 0);
		}
		return;
	}
	writeback(t, t->wb);
	b = find(target);
	if (b) {
		xjump(t, B_JP, entry_of(b));
		return;
	}
	{
		uint32_t site = exit_to(t, target);

		/* A link is only worth keeping for something that could one
		   day be translated. */
		if (good_target(t->s, target) && in_ram(t->s, target + 3) &&
		    can_do(fetch(t->s, target)))
			link_add(target, site);
	}
}

/* The block's inline code: the budget check if a loop comes back here, the
   charge, the body, and how it is left.  The check is before the charge, so a
   block always runs whole once it starts and `retired` stays exact even when
   the batch overruns -- by a block, or by however many fall-throughs it takes
   to reach the next check. */
/* A jump target on a word boundary: the queue fetches 32 bits at a time and
   a target in the upper half wastes the first fetch after every landing. */
static void align(struct tc *t)
{
	if (t->off & 2)
		w(t, 0);                        /* nop */
}

static void emit_block(struct tc *t, unsigned i)
{
	struct rblock *r = &rb[i];
	uint32_t ir = fetch(t->s, r->pc + 4 * (r->n - 1));
	uint32_t taken = succ_taken(t->s, r);
	int next_is_fall = i + 1 < t->nrb && rb[i + 1].pc == r->pc + 4 * r->n;
	unsigned j, u, unroll = 1;

	t->bidx = rv32_jit.blk_used + i;
	if (i == 0) {
		/* The region's own entry.  Nothing falls through into its
		   first block, so the loads go inline and a call or a return
		   lands once, not once on a trampoline and again here. */
		unsigned k;

		align(t);
		r->off = t->off;
		for (k = 0; k < NSLOT; ++k)
			if (t->map[k])
				getx(t, slot_reg[k], t->map[k]);
		r->warm = t->off;
		budget_check(t);
	} else {
		if (r->head)
			align(t);
		r->warm = t->off;
		if (r->head)
			budget_check(t);
	}

	/* A loop that is one block long is laid out several times over, each
	   copy leaving by a forward branch that is not taken while the loop
	   goes round.  Landing a taken branch in SDRAM costs twenty to thirty
	   cycles -- as much as the body of a small loop -- and this is what
	   makes most of the back edges fall-throughs instead. */
	unroll = r->unroll;

	for (u = 0; u < unroll && !t->full; ++u) {
		addimm(t, R6, r->n, 1);
		for (j = 0; j < r->n && !t->full; ++j) {
			t->i = j;
			insn(t, r->pc + 4 * j, fetch(t->s, r->pc + 4 * j));
		}
		if (u + 1 < unroll) {
			uint32_t site = jfar(t, cond_of((ir >> 12) & 7, 1));

			if (next_is_fall) {
				fix(&t->nfix, site, i + 1, FIX_BRANCH);
			} else {
				struct cold *c = cold_new(t, C_EXIT, 0, 0);

				c->code = r->pc + 4 * r->n;
				cold_site(c, site);
			}
		}
	}

	switch (r->term) {
	case T_JALR:
		writeback(t, t->wb);
		xjump(t, B_JP, (uint32_t)(uintptr_t)rv32_jit_stub_indirect);
		return;
	case T_BRANCH: {
		int j = in_region(t->nrb, taken);

		if (j >= 0) {
			/* Straight there: one prefixed branch. */
			uint32_t site = jfar(t, cond_of((ir >> 12) & 7, 0));

			if (rb[j].warm != 0xffffffffu)
				land_far(t, site, rb[j].warm);
			else
				fix(&t->nfix, site, (unsigned)j, FIX_BRANCH);
		} else {
			uint32_t skip = fwd(t, cond_of((ir >> 12) & 7, 1));

			region_jump(t, taken);
			land(t, skip);
		}
		break;
	}
	case T_JAL:
		if (i + 1 < t->nrb && rb[i + 1].pc == taken)
			return;         /* laid out next */
		region_jump(t, taken);
		return;
	default:
		break;
	}
	/* The fall-through, unless it is the block laid out next. */
	if (next_is_fall)
		return;
	region_jump(t, r->pc + 4 * r->n);
}

/* Translate the region at pc.  Returns where to enter, or NULL with *full
   set when the cache has no room for even its first block. */
static uint8_t *translate(rv32_t *s, uint32_t pc, int *full)
{
	struct tc t;
	unsigned nrb, i, k, total;
	uint32_t base;

	memset(&t, 0, sizeof t);
	t.s = s;
	t.base = rv32_jit.code;
	t.off = rv32_jit.code_used;
	t.end = rv32_jit.code_size;
	base = (uint32_t)(uintptr_t)t.base;
	*full = 0;

	nrb = discover(s, pc);
	if (nrb == 0)
		return NULL;

	/* Trim the region to what the cache and the tables have room for:
	   each block's bodies at the most an instruction can emit, and each
	   instruction's three cold entries a body.  Only the cache being
	   full is worth a flush; the cold table is sized so that a block of
	   BLOCK_CAP instructions always fits it. */
	for (;;) {
		total = 0;
		for (i = 0; i < nrb; ++i)
			total += rb[i].n * rb[i].unroll;
		if (t.off + total * INSN_ROOM + nrb * BLOCK_ROOM <= t.end &&
		    rv32_jit.blk_used + nrb <= rv32_jit.blk_max &&
		    3 * total + 4 * nrb + 4 <= COLD_MAX)
			break;
		if (--nrb == 0) {
			*full = 1;
			return NULL;
		}
	}

	choose(&t, nrb);
	t.nrb = nrb;
	t.nfix = 0;

	/* The cold entries: the map loaded, the batch checked, then into the
	   block.  Every path into the region comes through one of these,
	   except into its first block, whose entry is inline. */
	for (i = 0; i < nrb; ++i) {
		rb[i].warm = 0xffffffffu;
		if (i == 0)
			continue;
		align(&t);
		rb[i].off = t.off;
		t.bidx = rv32_jit.blk_used + i;
		for (k = 0; k < NSLOT; ++k)
			if (t.map[k])
				getx(&t, slot_reg[k], t.map[k]);
		budget_check(&t);
		fix(&t.nfix, t.off, i, FIX_JUMP);
		xjump(&t, B_JP, 0);
	}
	for (i = 0; i < nrb; ++i)
		emit_block(&t, i);
	emit_cold(&t);
	if (t.full) {
		*full = 1;
		return NULL;
	}
	for (i = 0; i < t.nfix; ++i) {
		uint32_t warm = rb[fixups[i].target].warm;

		if (fixups[i].kind == FIX_BRANCH) {
			land_far(&t, fixups[i].site, warm);
		} else {
			struct tc f;

			memset(&f, 0, sizeof f);
			f.base = t.base;
			f.off = fixups[i].site;
			f.end = f.off + 6;
			xjump(&f, B_JP, base + warm);
		}
	}
	for (i = 0; i < nrb; ++i)
		record(s, rb[i].pc, rb[i].off, rb[i].warm, rb[i].n, t.map,
		       rv32_jit.blk_used - i, nrb);

	rv32_jit.bytes += t.off - rv32_jit.code_used;
	rv32_jit.code_used = (t.off + 3) & ~3u;
	return t.base + rb[0].off;
}

/* ---- what the runtime calls -------------------------------------------- */

/* Retire a region: its blocks leave the tables, and each one's cold entry
   becomes an exit naming its own guest pc -- the same three words a link
   site has -- and goes on the link list.  Everything patched to jump to the
   old translation therefore keeps working, through the lookup, and is
   patched again to the new one the moment it exists. */
static void retire(uint32_t first, uint32_t n)
{
	uint32_t i;

	for (i = first; i < first + n; ++i) {
		struct jit_block *b = &rv32_jit.blk[i];
		uint32_t m = rv32_jit.blk_hash_mask;
		uint32_t h = (bhash(b->pc) >> 8) & m;
		struct tc t;

		while (rv32_jit.blk_hash[h] != (uint16_t)i)
			h = (h + 1) & m;
		rv32_jit.blk_hash[h] = H_DEAD;
		if (slot(b->pc)[0] == b->pc)
			slot(b->pc)[0] = 0;

		memset(&t, 0, sizeof t);
		t.base = rv32_jit.code;
		t.off = b->off;
		t.end = b->off + 12;
		link_add(b->pc, exit_to(&t, b->pc));
		b->pc = 0;
	}
	rv32_jit.stale++;
}

void rv32_jit_fence(rv32_t *s)
{
	uint32_t i = 0;

	rv32_jit.fences++;
	while (i < rv32_jit.blk_used) {
		uint32_t first = rv32_jit.blk[i].rfirst;
		uint32_t n = rv32_jit.blk[i].rn;
		uint32_t j;
		int stale = 0;

		for (j = first; j < first + n; ++j) {
			struct jit_block *b = &rv32_jit.blk[j];

			if (b->pc && checksum(s, b->pc, b->n) != b->sum)
				stale = 1;
		}
		if (stale && rv32_jit.blk[first].pc)
			retire(first, n);
		i = first + n;
	}
}

void rv32_jit_flush(void)
{
	memset(rv32_jit.map, 0, RV32_JIT_SLOTS * 2 * sizeof(uint32_t));
	memset(rv32_jit.hot, 0, RV32_JIT_HOT_SLOTS);
	memset(rv32_jit.blk_hash, 0xff,
	       (rv32_jit.blk_hash_mask + 1) * sizeof(uint16_t));
	memset(rv32_jit.link_hash, 0xff,
	       (rv32_jit.link_hash_mask + 1) * sizeof(uint16_t));
	rv32_jit.code_used = 0;
	rv32_jit.blk_used = 0;
	rv32_jit.link_used = 0;
	rv32_jit.flushes++;
}

uint8_t *rv32_jit_block(rv32_t *s, uint32_t pc)
{
	struct jit_block *b;
	uint8_t *code;
	int full;

	if (!good_target(s, pc) || !in_ram(s, pc + 3))
		return NULL;
	b = find(pc);
	if (b) {
		/* Put it back in the map on the way past: it is there to be
		   found by the runtime, and a collision took it out. */
		code = rv32_jit.code + b->off;
		slot(pc)[0] = pc;
		slot(pc)[1] = (uint32_t)(uintptr_t)code;
		return code;
	}
	/* An instruction this cannot translate is the caller's to interpret,
	   and nothing about the cache has to change for it.  Asking first is
	   what keeps a decline from looking like a cache that is full. */
	if (!can_do(fetch(s, pc)))
		return NULL;
	/* And code that has not been run enough to repay translating it is the
	   interpreter's, for now. */
	{
		uint8_t *seen = &rv32_jit.hot[(pc >> 8) & RV32_JIT_HOT_MASK];

		if (*seen < RV32_JIT_HOT) {
			++*seen;
			rv32_jit.warmups++;
			return NULL;
		}
	}
	code = translate(s, pc, &full);
	if (code || !full)
		return code;
	rv32_jit_flush();
	return translate(s, pc, &full);
}

uint32_t rv32_jit_fault(rv32_t *s, uint32_t code)
{
	struct jit_block *b = &rv32_jit.blk[code & 0xffff];
	unsigned i = (code >> 16) & 0xff, kind = code >> 24;
	uint32_t pc = b->pc + 4 * i;
	unsigned k;

	/* Whatever the block held in registers, x[] gets now. */
	for (k = 0; k < NSLOT; ++k)
		if (b->map[k])
			s->x[b->map[k]] = rv32_jit.spill[k];

	rv32_jit.back = 0;
	if (kind == RV32_JIT_BUDGET)
		return pc;                      /* nothing was charged yet */
	/* An address the translation cannot take, a target it cannot reach,
	   a width it cannot align.  The instruction itself is translatable,
	   so re-entering would fault in exactly the same place: this one is
	   the interpreter's, once.  It did not run, so it is given back along
	   with the rest of the block. */
	{
		uint32_t ir = fetch(s, pc);

		rv32_jit.back = b->n - i;
		rv32_jit.step = 1;
		rv32_jit.declines++;
		switch (ir & 0x7f) {
		case 0x03: case 0x23: {
			uint32_t a = s->x[(ir >> 15) & 31] +
				(uint32_t)((ir & 0x7f) == 0x03 ?
					   imm_i(ir) : imm_s(ir));

			rv32_jit.dec_mem++;
			if (in_ram(s, a))
				rv32_jit.dec_align++;
			else
				rv32_jit.dec_dev++;
			break;
		}
		case 0x63: case 0x67: rv32_jit.dec_jump++; break;
		default: break;
		}
	}
	return pc;
}

int rv32_jit_init(void *arena, uint32_t bytes)
{
	uint8_t *p = arena;
	uint8_t *end = (uint8_t *)arena + bytes;
	uint32_t nblk, nlink, hb, hl;

	if (bytes < 128u * 1024)
		return 0;

	memset(&rv32_jit, 0, sizeof rv32_jit);

	rv32_jit.map = rv32_jit_map;

	/* A block of this shape averages under a hundred bytes of code, so one
	   descriptor per hundred and sixty keeps the tables from being the
	   thing that fills up first without spending much on them. */
	nblk = (uint32_t)(end - p) / 160;
	if (nblk > H_DEAD - 1)
		nblk = H_DEAD - 1;
	nlink = nblk;
	for (hb = 1; hb < nblk * 2; hb <<= 1)
		;
	for (hl = 1; hl < nlink * 2; hl <<= 1)
		;

	rv32_jit.blk = (struct jit_block *)p;
	p += nblk * sizeof(struct jit_block);
	rv32_jit.link = (struct jit_link *)p;
	p += nlink * sizeof(struct jit_link);
	rv32_jit.blk_hash = (uint16_t *)p;
	p += hb * sizeof(uint16_t);
	rv32_jit.link_hash = (uint16_t *)p;
	p += hl * sizeof(uint16_t);
	rv32_jit.hot = p;
	p += RV32_JIT_HOT_SLOTS;

	rv32_jit.blk_max = nblk;
	rv32_jit.link_max = nlink;
	rv32_jit.blk_hash_mask = hb - 1;
	rv32_jit.link_hash_mask = hl - 1;

	p = (uint8_t *)(((uintptr_t)p + 3) & ~(uintptr_t)3);
	if (p >= end || (uint32_t)(end - p) < 64u * 1024)
		return 0;
	rv32_jit.code = p;
	rv32_jit.code_size = (uint32_t)(end - p);

	rv32_jit_flush();
	rv32_jit.flushes = 0;
	return 1;
}
