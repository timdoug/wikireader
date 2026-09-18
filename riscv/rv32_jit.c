/* rv32_jit.c - turn guest instructions into C33 instructions, once.
 *
 * The interpreter's cost is dominated by work that does not depend on the
 * machine at all: README.md measures 40.1 cycles of every 75 inside DISPATCH,
 * taking the instruction word apart and turning it into a table index and two
 * register offsets.  That is the same answer every time the instruction runs.
 * This does it once and keeps the answer.
 *
 * What it emits is deliberately the naive form jit_probe.s calls `alu_mem` and
 * `copy_mem`: every operand comes out of the guest register file and every
 * result goes back, because a register allocator is a separate piece of work
 * and this one has to be right first.  The device says that form is 2.8x the
 * interpreter and the allocated form 15x, so this is the smaller half of the
 * win and all of the machinery.
 *
 * Structure.  A *trace* is translated at a time, not a basic block: the
 * fall-through of a conditional branch and the target of an unconditional jump
 * are laid out immediately after their predecessor and cost nothing at all to
 * reach, which is what `exit_none` measures against `exit_link`'s 23.4 cycles.
 * A trace stops at an indirect jump, at an instruction this cannot translate,
 * or when it reaches code that already exists.  Each basic block inside it is
 * still registered by its own guest pc, so a jump into the middle of a trace
 * finds it.
 *
 * Exits that cannot be laid out inline are three words of `xld.w %r13,pc`
 * followed by a jump to the runtime's lookup.  When the target is translated
 * those three words are overwritten with a direct jump to it -- the same size,
 * so nothing has to move.
 *
 * Giving up.  A translated block leaves through one of three stubs, and none
 * of them carries the guest pc: the stub reads the return address the C33's
 * `call` pushes and rv32_jit_fault() translates the block again, with nothing
 * written anywhere, to find which guest instruction owns that byte.  Emitted
 * sizes depend only on the instruction word, never on where the code lands or
 * on what else has been translated, so the replay always agrees with the
 * original.  That is worth three words at every check instead of the eight a
 * block would need to keep its own pc up to date.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "rv32.h"
#include "rv32_jit.h"

rv32_jit_t rv32_jit;

/* The offsets only have to hold where the assembly runs; the build machine's
   pointers are twice the size and put the two words somewhere else. */
#if __SIZEOF_POINTER__ == 4
_Static_assert(offsetof(rv32_jit_t, map) == RV32_JOFF_MAP, "asm offset");
_Static_assert(offsetof(rv32_jit_t, watch_lo) == RV32_JOFF_WATCH_LO, "asm offset");
_Static_assert(offsetof(rv32_jit_t, watch_hi) == RV32_JOFF_WATCH_HI, "asm offset");
_Static_assert(offsetof(rv32_jit_t, back) == RV32_JOFF_BACK, "asm offset");
#endif

#define NO_RESERVATION 0xffffffffu

/* The runtime, in rv32_jit.s.  Each stub is the target of a call from inside
   the code cache and never returns to it. */
void rv32_jit_stub_decline(void);
void rv32_jit_stub_store(void);
void rv32_jit_stub_dev_load(void);
void rv32_jit_stub_dev_store(void);
void rv32_jit_stub_budget(void);
void rv32_jit_stub_indirect(void);

/* rv32.c, for the operations with no C33 instruction behind them. */
uint32_t rv32_divop(uint32_t funct3, uint32_t a, uint32_t b);

struct jit_block {
	uint32_t pc;     /* the guest pc it starts at */
	uint32_t off;    /* where its code starts in the cache */
	uint32_t mark;   /* where its instruction sizes start */
	uint16_t n;      /* guest instructions in it */
	uint16_t pad;
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

enum {                           /* register forms */
	O_MOV   = 0x2e,          /* ld.w  %ra,%rb        */
	O_LDB   = 0x20, O_LDUB = 0x24, O_LDH = 0x28, O_LDUH = 0x2c, O_LDW = 0x30,
	O_STB   = 0x34, O_STH  = 0x38, O_STW = 0x3c,
	O_ADD   = 0x22, O_SUB  = 0x26, O_CMP = 0x2a,
	O_AND   = 0x32, O_OR   = 0x36, O_XOR = 0x3a, O_NOT = 0x3e,
	O_SRL   = 0x89, O_SLL  = 0x8d, O_SRA = 0x91,
	O_MLT   = 0xaa, O_MLTU = 0xae,
	O_SPEC  = 0xa4,          /* ld.w  %ra,%alr (b=2) / %ahr (b=3) */
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
static const uint16_t shift_op[3][2] = {
	{ 0x8800, 0x2300 },      /* srl */
	{ 0x8c00, 0x2700 },      /* sll */
	{ 0x9000, 0x2b00 },      /* sra */
};
enum { S_SRL = 0, S_SLL = 1, S_SRA = 2 };

enum {                           /* branches */
	B_GT = 0x08, B_GE = 0x0a, B_LT = 0x0c, B_LE = 0x0e,
	B_UGT = 0x10, B_UGE = 0x12, B_ULT = 0x14, B_ULE = 0x16,
	B_EQ = 0x18, B_NE = 0x1a, B_CALL = 0x1c, B_JP = 0x1e,
};

struct tc {
	rv32_t   *s;
	uint8_t  *base;     /* where the code will run */
	uint32_t  off;
	uint32_t  end;      /* the offset it must stop before */
	int       full;
};

static void w(struct tc *t, unsigned v)
{
	if (t->off + 2 > t->end) {
		t->full = 1;
		return;
	}
	t->base[t->off] = (uint8_t)v;
	t->base[t->off + 1] = (uint8_t)(v >> 8);
	t->off += 2;
}

static uint32_t run_at(struct tc *t)
{
	return (uint32_t)(uintptr_t)t->base + t->off;
}

static void ext(struct tc *t, uint32_t v)
{
	w(t, 0xc000u | (v & 0x1fffu));
}

static void rr(struct tc *t, unsigned op, unsigned a, unsigned b)
{
	w(t, (op << 8) | (b << 4) | a);
}

/* An immediate wide enough to need prefixes gets both of them whenever it does
   not fit nineteen bits signed, because one prefix composes a 19-bit field the
   core then sign-extends. */
static void ri(struct tc *t, unsigned op, unsigned a, uint32_t v)
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
static void addimm(struct tc *t, unsigned rd, uint32_t v, int sub)
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
static void shift(struct tc *t, unsigned op, unsigned a, unsigned n)
{
	n &= 0x1f;
	w(t, shift_op[op][n >= 16] | ((n & 0xf) << 4) | a);
}

/* A jump or call to an address outside the reach of the eight-bit field.
   Always three words, which is what lets a link be patched in place. */
static void xjump(struct tc *t, unsigned op, uint32_t target)
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
static uint32_t fwd(struct tc *t, unsigned op)
{
	uint32_t at = t->off;

	w(t, op << 8);
	return at;
}

static void land(struct tc *t, uint32_t at)
{
	if (t->full)
		return;
	t->base[at] = (uint8_t)((t->off - at) / 2);
}

/* ---- the guest register file ------------------------------------------ */

static void getx(struct tc *t, unsigned h, unsigned r)
{
	if (r == 0) {
		ri(t, I_MOV, h, 0);
		return;
	}
	ext(t, r * 4);
	rr(t, O_LDW, h, R0);
}

static void putx(struct tc *t, unsigned r, unsigned h)
{
	if (r == 0)
		return;
	ext(t, r * 4);
	rr(t, O_STW, h, R0);
}

/* ---- guest instructions ------------------------------------------------ */

static uint32_t fetch(rv32_t *s, uint32_t pc)
{
	const uint8_t *p = s->ram + (pc - RV_RAM_BASE);

	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int in_ram(rv32_t *s, uint32_t pc)
{
	return pc >= RV_RAM_BASE && pc - RV_RAM_BASE < s->ram_size;
}

static int good_target(rv32_t *s, uint32_t pc)
{
	return !(pc & 3) && in_ram(s, pc);
}

static int32_t imm_i(uint32_t ir)  { return (int32_t)ir >> 20; }
static int32_t imm_s(uint32_t ir)
{
	return (int32_t)((ir & 0xfe000000u)) >> 20 | (int32_t)((ir >> 7) & 0x1f);
}
static int32_t imm_b(uint32_t ir)
{
	return (int32_t)((ir & 0x80000000u)) >> 19 |
	       (int32_t)((ir >> 7) & 0x1e) |
	       (int32_t)((ir >> 20) & 0x7e0) |
	       (int32_t)((ir << 4) & 0x800);
}
static int32_t imm_j(uint32_t ir)
{
	return (int32_t)((ir & 0x80000000u)) >> 11 |
	       (int32_t)(ir & 0xff000) |
	       (int32_t)((ir >> 9) & 0x800) |
	       (int32_t)((ir >> 20) & 0x7fe);
}

/* Can this be translated at all?  Everything rare stays in the C interpreter,
   for the same reason the assembly hot path leaves it there: it is written
   once, where it can be read. */
/* A bisecting switch: each bit takes one class of guest instruction away from
   the translator and gives it to the C interpreter, which is known good.  It
   exists because a translation that is wrong on one opcode boots Linux into
   the weeds a million instructions later, and this says which opcode in one
   run apiece. */
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
	default:
		return 0;
	}
}

/* The three ways a guest address can go, and the branch sites that reach them.
   Checked in the order rv32_hot.s checks them: misaligned first, because that
   branch is never taken, then both ends of guest RAM. */
struct paths {
	uint32_t dev[2];        /* outside RAM: a device register */
	unsigned ndev;
	uint32_t bad;           /* misaligned: a trap, and C's */
	int      has_bad;
	uint32_t ok;            /* inside RAM: the ordinary access */
};

static void address(struct tc *t, unsigned rs1, int32_t off, unsigned align,
		    struct paths *p)
{
	getx(t, R4, rs1);
	if (off)
		addimm(t, R4, (uint32_t)off, 0);
	rr(t, O_ADD, R4, R8);
	p->ndev = 0;
	p->has_bad = 0;
	if (align) {
		rr(t, O_MOV, R13, R4);
		ri(t, I_AND, R13, align);
		p->bad = fwd(t, B_NE);
		p->has_bad = 1;
	}
	rr(t, O_CMP, R4, R2);
	p->dev[p->ndev++] = fwd(t, B_ULT);
	rr(t, O_CMP, R4, R3);
	p->ok = fwd(t, B_ULT);
}

/* What follows the checks: the device call the two out-of-range branches reach,
   a jump over the ordinary access to rejoin it, and the decline a misaligned
   one takes.  A device register is 0.8% of what a Linux boot executes and two
   thirds of everything this would otherwise hand back to C -- handing one back
   costs a return, an instruction interpreted from SDRAM, a lookup and a
   re-entry, and doing it here instead is the single largest thing between this
   translator and the interpreter it replaces. */
static uint32_t device_path(struct tc *t, struct paths *p, uint32_t stub)
{
	uint32_t rejoin;

	while (p->ndev--)
		land(t, p->dev[p->ndev]);
	xjump(t, B_CALL, stub);
	rejoin = fwd(t, B_JP);
	if (p->has_bad) {
		land(t, p->bad);
		xjump(t, B_CALL,
		      (uint32_t)(uintptr_t)rv32_jit_stub_decline);
	}
	land(t, p->ok);
	return rejoin;
}

/* x[rd] = x[rs1] < b, for one of the two orderings.  The comparison has
   already been made; this only turns the flags into a word. */
static void setless(struct tc *t, unsigned rd, unsigned nge)
{
	uint32_t over;

	ri(t, I_MOV, R13, 0);
	over = fwd(t, nge);
	ri(t, I_MOV, R13, 1);
	land(t, over);
	putx(t, rd, R13);
}

/* The M operations with no C33 instruction behind them.  %r0..%r3 survive a
   call, so only the loop's own registers have to be saved -- and %r8, %r9 and
   %r10 are the argument registers, which is why all three go on the stack. */
static void mcall(struct tc *t, unsigned rd, unsigned f3)
{
	w(t, (0x84 << 8) | 4);                  /* sub %sp,0x4 -- four words */
	w(t, (0x17 << 10) | (0 << 4) | R6);     /* ld.w [%sp+0x0],%r6 */
	w(t, (0x17 << 10) | (1 << 4) | R8);
	w(t, (0x17 << 10) | (2 << 4) | R9);
	w(t, (0x17 << 10) | (3 << 4) | R10);
	ri(t, I_MOV, R6, f3);
	rr(t, O_MOV, R7, R4);
	rr(t, O_MOV, R8, R5);
	xjump(t, B_CALL, (uint32_t)(uintptr_t)rv32_divop);
	w(t, (0x14 << 10) | (0 << 4) | R6);     /* ld.w %r6,[%sp+0x0] */
	w(t, (0x14 << 10) | (1 << 4) | R8);
	w(t, (0x14 << 10) | (2 << 4) | R9);
	w(t, (0x14 << 10) | (3 << 4) | R10);
	w(t, (0x80 << 8) | 4);                  /* add %sp,0x4 */
	putx(t, rd, R4);
}

/* One guest instruction.  Control transfers emit everything except how the
   block is left, which the trace builder decides. */
static void insn(struct tc *t, uint32_t pc, uint32_t ir)
{
	unsigned op = ir & 0x7f;
	unsigned rd = (ir >> 7) & 31, rs1 = (ir >> 15) & 31, rs2 = (ir >> 20) & 31;
	unsigned f3 = (ir >> 12) & 7, f7 = ir >> 25;

	switch (op) {
	case 0x37:                                      /* lui */
		if (rd) {
			ri(t, I_MOV, R4, ir & 0xfffff000u);
			putx(t, rd, R4);
		}
		return;

	case 0x17:                                      /* auipc */
		if (rd) {
			ri(t, I_MOV, R4, pc + (ir & 0xfffff000u));
			putx(t, rd, R4);
		}
		return;

	case 0x6f:                                      /* jal */
		if (rd) {
			ri(t, I_MOV, R4, pc + 4);
			putx(t, rd, R4);
		}
		return;                                 /* the trace does the jump */

	case 0x67: {                                    /* jalr */
		uint32_t jbad, jlow, jok;

		getx(t, R4, rs1);
		if (imm_i(ir))
			addimm(t, R4, (uint32_t)imm_i(ir), 0);
		ri(t, I_AND, R4, 0xfffffffeu);          /* bit 0 is cleared, not a fault */
		/* The target has to land inside RAM on a word boundary; anything
		   else is a trap, and traps are built in one place. */
		rr(t, O_MOV, R5, R4);
		ri(t, I_AND, R5, 3);
		jbad = fwd(t, B_NE);
		rr(t, O_MOV, R5, R4);
		rr(t, O_ADD, R5, R8);
		rr(t, O_CMP, R5, R2);
		jlow = fwd(t, B_ULT);
		rr(t, O_CMP, R5, R3);
		jok = fwd(t, B_ULT);
		land(t, jbad);
		land(t, jlow);
		xjump(t, B_CALL, (uint32_t)(uintptr_t)rv32_jit_stub_decline);
		land(t, jok);
		if (rd) {
			ri(t, I_MOV, R5, pc + 4);
			putx(t, rd, R5);
		}
		rr(t, O_MOV, R13, R4);
		return;                                 /* the trace does the jump */
	}

	case 0x63:                                      /* branch */
		getx(t, R4, rs1);
		getx(t, R5, rs2);
		rr(t, O_CMP, R4, R5);
		return;                                 /* the trace does the jump */

	case 0x03: {                                    /* load */
		static const unsigned kind[8] = {
			O_LDB, O_LDH, O_LDW, 0, O_LDUB, O_LDUH, 0, 0
		};
		struct paths p;
		uint32_t rejoin;

		address(t, rs1, imm_i(ir),
			f3 == 2 ? 3 : (f3 == 1 || f3 == 5) ? 1 : 0, &p);
		rejoin = device_path(t, &p,
				     (uint32_t)(uintptr_t)rv32_jit_stub_dev_load);
		rr(t, kind[f3], R5, R4);
		land(t, rejoin);        /* the device left its word in %r5 too */
		putx(t, rd, R5);
		return;
	}

	case 0x23: {                                    /* store */
		static const unsigned kind[4] = { O_STB, O_STH, O_STW, 0 };
		uint32_t below, above, rejoin;
		struct paths p;

		getx(t, R5, rs2);               /* before %r13 is the align scratch */
		address(t, rs1, imm_s(ir), f3 == 2 ? 3 : f3 == 1 ? 1 : 0, &p);
		rejoin = device_path(t, &p,
				     (uint32_t)(uintptr_t)rv32_jit_stub_dev_store);
		rr(t, kind[f3], R5, R4);        /* the value first, the base second */
		/* A store inside the watched range is either into code that has
		   been translated or into a word some load reserved.  Both are
		   rare and both belong to C; with nothing watched the range is
		   empty and neither branch is taken. */
		rr(t, O_CMP, R4, R9);
		below = fwd(t, B_ULT);
		rr(t, O_CMP, R4, R10);
		above = fwd(t, B_UGE);
		xjump(t, B_CALL, (uint32_t)(uintptr_t)rv32_jit_stub_store);
		land(t, below);
		land(t, above);
		land(t, rejoin);        /* a device store is already done */
		return;
	}

	case 0x13:                                      /* op-imm */
		getx(t, R4, rs1);
		switch (f3) {
		case 0: if (imm_i(ir)) addimm(t, R4, (uint32_t)imm_i(ir), 0); break;
		case 1: shift(t, S_SLL, R4, rs2); break;
		case 2: ri(t, I_CMP, R4, (uint32_t)imm_i(ir));
			setless(t, rd, B_GE);
			return;
		case 3: ri(t, I_CMP, R4, (uint32_t)imm_i(ir));
			setless(t, rd, B_UGE);
			return;
		case 4: ri(t, I_XOR, R4, (uint32_t)imm_i(ir)); break;
		case 5: shift(t, f7 == 0x20 ? S_SRA : S_SRL, R4, rs2); break;
		case 6: ri(t, I_OR,  R4, (uint32_t)imm_i(ir)); break;
		default: ri(t, I_AND, R4, (uint32_t)imm_i(ir)); break;
		}
		putx(t, rd, R4);
		return;

	case 0x33:                                      /* op */
		getx(t, R4, rs1);
		getx(t, R5, rs2);
		if (f7 == 1) {
			switch (f3) {
			case 0: rr(t, O_MLTU, R4, R5);
				rr(t, O_SPEC, R4, 2);   /* ld.w %r4,%alr */
				break;
			case 1: rr(t, O_MLT, R4, R5);
				rr(t, O_SPEC, R4, 3);   /* ld.w %r4,%ahr */
				break;
			case 3: rr(t, O_MLTU, R4, R5);
				rr(t, O_SPEC, R4, 3);
				break;
			default:                        /* mulhsu and the divides */
				mcall(t, rd, f3);
				return;
			}
			putx(t, rd, R4);
			return;
		}
		switch (f3) {
		case 0: rr(t, f7 == 0x20 ? O_SUB : O_ADD, R4, R5); break;
		case 1: ri(t, I_AND, R5, 31); rr(t, O_SLL, R4, R5); break;
		case 2: rr(t, O_CMP, R4, R5); setless(t, rd, B_GE); return;
		case 3: rr(t, O_CMP, R4, R5); setless(t, rd, B_UGE); return;
		case 4: rr(t, O_XOR, R4, R5); break;
		case 5: ri(t, I_AND, R5, 31);
			rr(t, f7 == 0x20 ? O_SRA : O_SRL, R4, R5);
			break;
		case 6: rr(t, O_OR,  R4, R5); break;
		default: rr(t, O_AND, R4, R5); break;
		}
		putx(t, rd, R4);
		return;

	default:                                        /* fence: nothing is
							   cached or reordered */
		return;
	}
}

/* ---- blocks, traces and the cache -------------------------------------- */

/* A block ends at the first control transfer, at the first instruction this
   cannot translate, or at the cap -- whichever comes first.  Nothing is
   emitted here; the count is what the prologue charges the batch for. */
enum { T_BRANCH, T_JAL, T_JALR, T_DECLINE, T_CAP };

#define BLOCK_CAP 200
#define TRACE_CAP 1000
#define INSN_ROOM 96        /* the largest any one instruction emits */

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
		case 0x63: *term = T_BRANCH; return n + 1;
		case 0x6f: *term = T_JAL;    return n + 1;
		case 0x67: *term = T_JALR;   return n + 1;
		default: break;
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

static struct jit_block *find(uint32_t pc)
{
	uint32_t m = rv32_jit.blk_hash_mask;
	uint32_t i = (bhash(pc) >> 8) & m;

	for (;;) {
		uint16_t v = rv32_jit.blk_hash[i];

		if (v == 0xffff)
			return NULL;
		if (rv32_jit.blk[v].pc == pc)
			return &rv32_jit.blk[v];
		i = (i + 1) & m;
	}
}

static void patch(uint32_t off, uint32_t code)
{
	struct tc t = { 0 };

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

static void link_resolve(uint32_t pc, uint32_t code)
{
	uint32_t m = rv32_jit.link_hash_mask;
	uint32_t i = (bhash(pc) >> 8) & m;
	uint32_t at = rv32_jit.link_hash[i] == 0xffff ? 0xffffffffu :
		      rv32_jit.link_hash[i];

	while (at != 0xffffffffu) {
		struct jit_link *l = &rv32_jit.link[at];

		if (l->pc == pc && l->off != 0xffffffffu) {
			patch(l->off, code);
			l->off = 0xffffffffu;
		}
		at = l->next;
	}
}

static struct jit_block *record(rv32_t *s, uint32_t pc, uint32_t off, unsigned n)
{
	struct jit_block *b;
	uint32_t m, i;

	if (rv32_jit.blk_used >= rv32_jit.blk_max ||
	    rv32_jit.mark_used + n > rv32_jit.mark_max)
		return NULL;
	b = &rv32_jit.blk[rv32_jit.blk_used];
	b->pc = pc;
	b->off = off;
	b->mark = rv32_jit.mark_used;
	b->n = (uint16_t)n;
	b->pad = 0;
	rv32_jit.mark_used += n;

	m = rv32_jit.blk_hash_mask;
	i = (bhash(pc) >> 8) & m;
	while (rv32_jit.blk_hash[i] != 0xffff)
		i = (i + 1) & m;
	rv32_jit.blk_hash[i] = (uint16_t)rv32_jit.blk_used;
	rv32_jit.blk_used++;

	slot(pc)[0] = pc;
	slot(pc)[1] = (uint32_t)(uintptr_t)(rv32_jit.code + off);

	if (rv32_jit.code_hi == rv32_jit.code_lo) {
		rv32_jit.code_lo = pc;
		rv32_jit.code_hi = pc + 4 * n;
	} else {
		if (pc < rv32_jit.code_lo)
			rv32_jit.code_lo = pc;
		if (pc + 4 * n > rv32_jit.code_hi)
			rv32_jit.code_hi = pc + 4 * n;
	}
	rv32_jit_reserve(s);
	link_resolve(pc, (uint32_t)(uintptr_t)(rv32_jit.code + off));
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

/* The prologue: stop if the batch is spent, then charge it for this block.
   The check is before the subtraction, so a block always runs whole once it
   starts and `retired` stays exact even when the batch overruns. */
static void prologue(struct tc *t, unsigned n)
{
	uint32_t on;

	ri(t, I_CMP, R6, 0);
	on = fwd(t, B_GT);
	xjump(t, B_CALL, (uint32_t)(uintptr_t)rv32_jit_stub_budget);
	land(t, on);
	addimm(t, R6, n, 1);
}

/* Emit a block and record how many bytes each guest instruction became.  One
   byte apiece is all it takes -- the largest any instruction emits is under a
   hundred -- and it is what turns finding the guest pc behind a fault from a
   second translation into a walk.  The first entry carries the prologue too. */
static void emit_block(struct tc *t, struct jit_block *b)
{
	uint32_t pc = b->pc;
	uint32_t was = t->off;
	unsigned i;

	prologue(t, b->n);
	for (i = 0; i < b->n; ++i) {
		insn(t, pc + 4 * i, fetch(t->s, pc + 4 * i));
		if (t->full)
			return;
		rv32_jit.mark[b->mark + i] = (uint8_t)(t->off - was);
		was = t->off;
	}
}

/* Inverted, because the block that follows is laid out next: a guest branch
   that is taken falls into its exit and one that is not jumps over it. */
static unsigned inverse(unsigned f3)
{
	switch (f3) {
	case 0: return B_NE;            /* beq  */
	case 1: return B_EQ;            /* bne  */
	case 4: return B_GE;            /* blt  */
	case 5: return B_LT;            /* bge  */
	case 6: return B_UGE;           /* bltu */
	default: return B_ULT;          /* bgeu */
	}
}

static uint8_t *code_at(uint32_t pc)
{
	struct jit_block *b = find(pc);

	return b ? rv32_jit.code + b->off : NULL;
}

/* Lay out a trace: blocks chained head to tail for as long as each one's
   successor is known and not already translated.  Returns where to enter. */
static uint8_t *translate(rv32_t *s, uint32_t pc)
{
	struct tc t = { 0 };
	uint8_t *entry;
	unsigned done = 0;

	t.s = s;
	t.base = rv32_jit.code;
	t.off = rv32_jit.code_used;
	t.end = rv32_jit.code_size;
	entry = rv32_jit.code + t.off;

	for (;;) {
		int term;
		unsigned n = scan(s, pc, &term);
		uint32_t ir, target;

		if (n == 0) {
			if (done)
				exit_to(&t, pc);
			break;
		}
		if (t.off + n * INSN_ROOM + INSN_ROOM > t.end) {
			if (done)
				exit_to(&t, pc);
			else
				t.full = 1;
			break;
		}
		{
			struct jit_block *b = record(s, pc, t.off, n);

			if (!b) {
				if (done)
					exit_to(&t, pc);
				break;
			}
			emit_block(&t, b);
		}
		done += n;

		ir = fetch(s, pc + 4 * (n - 1));
		switch (term) {
		case T_JALR:
			xjump(&t, B_JP,
			      (uint32_t)(uintptr_t)rv32_jit_stub_indirect);
			target = 0;
			break;
		case T_JAL:
			target = pc + 4 * (n - 1) + (uint32_t)imm_j(ir);
			if (!good_target(s, target)) {
				/* The jump itself faults; let C raise it. */
				exit_to(&t, pc + 4 * (n - 1));
				target = 0;
			}
			break;
		case T_BRANCH: {
			uint32_t taken = pc + 4 * (n - 1) + (uint32_t)imm_b(ir);
			uint32_t over = fwd(&t, inverse((ir >> 12) & 7));
			uint32_t site;

			if (good_target(s, taken)) {
				uint8_t *c = code_at(taken);

				site = exit_to(&t, taken);
				if (c)
					patch(site, (uint32_t)(uintptr_t)c);
				else
					link_add(taken, site);
			} else {
				xjump(&t, B_CALL,
				      (uint32_t)(uintptr_t)rv32_jit_stub_decline);
			}
			land(&t, over);
			target = pc + 4 * n;
			break;
		}
		default:                /* T_CAP, and T_DECLINE with work behind it */
			target = pc + 4 * n;
			break;
		}

		if (t.full)
			break;
		if (target == 0 || !good_target(s, target))
			break;
		{
			uint8_t *c = code_at(target);

			if (c) {
				patch(exit_to(&t, target),
				      (uint32_t)(uintptr_t)c);
				break;
			}
		}
		if (done >= TRACE_CAP) {
			link_add(target, exit_to(&t, target));
			break;
		}
		pc = target;
	}

	if (t.full || !done)
		return NULL;
	rv32_jit.bytes += t.off - rv32_jit.code_used;
	rv32_jit.code_used = (t.off + 3) & ~3u;
	return entry;
}

/* ---- what the runtime calls -------------------------------------------- */

void rv32_jit_reserve(rv32_t *s)
{
	uint32_t base = (uint32_t)(uintptr_t)s->ram;

	if (s->reservation != NO_RESERVATION) {
		/* Every store has to be seen until the reservation is settled,
		   which is what widening the watched range to all of guest RAM
		   does -- no extra instruction anywhere, and the sequences this
		   costs anything in are three instructions long. */
		rv32_jit.watch_lo = base;
		rv32_jit.watch_hi = base + s->ram_size;
	} else if (rv32_jit.code_hi != rv32_jit.code_lo) {
		rv32_jit.watch_lo = base + (rv32_jit.code_lo - RV_RAM_BASE);
		rv32_jit.watch_hi = base + (rv32_jit.code_hi - RV_RAM_BASE);
	} else {
		rv32_jit.watch_lo = 0;
		rv32_jit.watch_hi = 0;
	}
}

void rv32_jit_flush(void)
{
	memset(rv32_jit.map, 0, RV32_JIT_SLOTS * 2 * sizeof(uint32_t));
	memset(rv32_jit.blk_hash, 0xff,
	       (rv32_jit.blk_hash_mask + 1) * sizeof(uint16_t));
	memset(rv32_jit.link_hash, 0xff,
	       (rv32_jit.link_hash_mask + 1) * sizeof(uint16_t));
	rv32_jit.code_used = 0;
	rv32_jit.blk_used = 0;
	rv32_jit.mark_used = 0;
	rv32_jit.link_used = 0;
	rv32_jit.code_lo = rv32_jit.code_hi = 0;
	rv32_jit.watch_lo = rv32_jit.watch_hi = 0;
	rv32_jit.flushes++;
}

uint8_t *rv32_jit_block(rv32_t *s, uint32_t pc)
{
	uint8_t *code;

	if (!good_target(s, pc) || !in_ram(s, pc + 3))
		return NULL;
	code = code_at(pc);
	if (code) {
		/* Put it back in the map on the way past: it is there to be
		   found by the runtime, and a collision took it out. */
		slot(pc)[0] = pc;
		slot(pc)[1] = (uint32_t)(uintptr_t)code;
		return code;
	}
	/* An instruction this cannot translate is the caller's to interpret,
	   and nothing about the cache has to change for it.  Asking first is
	   what keeps a decline -- one instruction in 78 -- from looking like a
	   cache that is full. */
	if (!can_do(fetch(s, pc)))
		return NULL;
	code = translate(s, pc);
	if (code)
		return code;
	rv32_jit_flush();
	rv32_jit_reserve(s);
	return translate(s, pc);
}

/* Which guest instruction owns a byte of the code cache.  The block is found
   by where its code starts and then walked by the sizes emit_block() wrote
   down.  It used to translate the block a second time to find out, which is
   correct and was 93% of a Linux boot: a decline happens once in every seventy
   instructions and translating is thousands of cycles. */
static uint32_t pc_of(rv32_t *s, uint32_t off, unsigned *ahead)
{
	uint32_t lo = 0, hi = rv32_jit.blk_used;
	struct jit_block *b;
	uint32_t at;
	unsigned i;

	*ahead = 0;
	if (!rv32_jit.blk_used)
		return s->pc;
	while (lo + 1 < hi) {
		uint32_t mid = (lo + hi) / 2;

		if (rv32_jit.blk[mid].off <= off)
			lo = mid;
		else
			hi = mid;
	}
	b = &rv32_jit.blk[lo];

	at = b->off;
	for (i = 0; i + 1 < b->n; ++i) {
		at += rv32_jit.mark[b->mark + i];
		if (off <= at)
			break;
	}
	/* What follows the last instruction's code is how the block is left,
	   and that is that instruction's doing too. */
	*ahead = b->n - i;
	return b->pc + 4 * i;
}

uint32_t rv32_jit_fault(rv32_t *s, uint32_t ra, uint32_t kind, uint32_t addr)
{
	uint32_t base = (uint32_t)(uintptr_t)rv32_jit.code;
	unsigned ahead;
	uint32_t pc;

	rv32_jit.back = 0;
	if (ra < base || ra - base >= rv32_jit.code_size)
		return s->pc;
	pc = pc_of(s, ra - base, &ahead);
	if (kind == RV32_JIT_BUDGET)
		return pc;                      /* nothing was charged yet */
	if (kind == RV32_JIT_DECLINE) {
		/* An address the translation cannot take, a target it cannot
		   reach, a width it cannot align.  The instruction itself is
		   translatable, so re-entering would fault in exactly the same
		   place: this one is the interpreter's, once.  It did not run,
		   so it is given back along with the rest of the block. */
		rv32_jit.back = ahead;
		rv32_jit.step = 1;
		rv32_jit.declines++;
		{
			uint32_t ir = fetch(s, pc);

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
	if (kind != RV32_JIT_STORE)
		return pc;
	rv32_jit.back = ahead - 1;              /* the store itself did run */
	rv32_jit.stores++;

	/* The store has already happened, so the guest resumes after it. */
	{
		uint32_t g = addr - (uint32_t)(uintptr_t)s->ram + RV_RAM_BASE;

		if ((g & ~3u) == s->reservation)
			s->reservation = NO_RESERVATION;
		if (g >= rv32_jit.code_lo && g < rv32_jit.code_hi)
			rv32_jit_flush();
		rv32_jit_reserve(s);
	}
	return pc + 4;
}

int rv32_jit_init(void *arena, uint32_t bytes)
{
	uint8_t *p = arena;
	uint8_t *end = (uint8_t *)arena + bytes;
	uint32_t nblk, nlink, hb, hl;

	if (bytes < 128u * 1024)
		return 0;

	memset(&rv32_jit, 0, sizeof rv32_jit);

	rv32_jit.map = (uint32_t *)p;
	p += RV32_JIT_SLOTS * 2 * sizeof(uint32_t);

	/* A block of this shape averages about a hundred bytes of code, so one
	   descriptor per hundred and sixty keeps the tables from being the
	   thing that fills up first without spending much on them. */
	nblk = (uint32_t)(end - p) / 160;
	if (nblk > 0xfffe)
		nblk = 0xfffe;
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
	/* One byte a guest instruction, and a block of this shape averages
	   about thirty-five bytes of code an instruction. */
	rv32_jit.mark = p;
	rv32_jit.mark_max = ((uint32_t)(end - p) / 24) & ~3u;
	p += rv32_jit.mark_max;         /* a multiple of four: what follows is
					   halfwords, and this core traps a
					   misaligned one */
	rv32_jit.link_hash = (uint16_t *)p;
	p += hl * sizeof(uint16_t);

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
