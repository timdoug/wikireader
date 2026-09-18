/* rv32.c - the rv32ima interpreter.
 *
 * Plain portable C on purpose: this is the correctness reference and the
 * thing the first cycle measurements are taken against.  Every C33-specific
 * placement decision comes after there is a number to compare it with.
 *
 * Guest RAM is a flat host array; a guest physical address is an index into
 * it once RV_RAM_BASE is subtracted.  Both the array and every word access
 * are 4-byte aligned, because the C33 traps unaligned accesses and rv32
 * without the C extension never asks for one.
 */

#include <stddef.h>

#include "rv32.h"
#ifdef RV32_JIT
#include "rv32_jit.h"
#endif

/* The offsets only have to hold where the assembly runs; the host
   reference build has 64-bit pointers and a different layout. */
#if __SIZEOF_POINTER__ == 4
_Static_assert(offsetof(rv32_t, x) == RV32_OFF_X, "asm offset");
_Static_assert(offsetof(rv32_t, pc) == RV32_OFF_PC, "asm offset");
_Static_assert(offsetof(rv32_t, ram) == RV32_OFF_RAM, "asm offset");
_Static_assert(offsetof(rv32_t, ram_size) == RV32_OFF_RAM_SIZE, "asm offset");
_Static_assert(offsetof(rv32_t, reservation) == RV32_OFF_RESERVE, "asm offset");
#endif

/* mstatus bits we model. */
enum {
	MSTATUS_MIE  = 1u << 3,
	MSTATUS_MPIE = 1u << 7,
	MSTATUS_MPP  = 3u << 11,
};

/* Interrupt/exception causes. */
enum {
	CAUSE_MACHINE_TIMER    = 0x80000007,
	CAUSE_INSN_MISALIGNED  = 0,
	CAUSE_INSN_FAULT       = 1,
	CAUSE_ILLEGAL_INSN     = 2,
	CAUSE_BREAKPOINT       = 3,
	CAUSE_LOAD_MISALIGNED  = 4,
	CAUSE_LOAD_FAULT       = 5,
	CAUSE_STORE_MISALIGNED = 6,
	CAUSE_STORE_FAULT      = 7,
	CAUSE_ECALL_U          = 8,
	CAUSE_ECALL_M          = 11,
};

#define MIP_MTIP (1u << 7)
#define MIE_MTIE (1u << 7)

#define NO_RESERVATION 0xffffffffu

/* A trap in flight.  0 means none; otherwise cause+1 for exceptions so that
   cause 0 (instruction misaligned) is still distinguishable from "no trap",
   and the raw value for interrupts, whose top bit is already set. */
#define TRAP_EXC(cause) ((cause) + 1)
#define TRAP_IS_INTERRUPT(t) ((t) & 0x80000000u)

void rv32_reset(rv32_t *s, uint32_t entry, uint32_t dtb)
{
	for (int i = 0; i < 32; ++i)
		s->x[i] = 0;
	s->pc = entry;
	s->x[10] = 0;    /* a0: hart id */
	s->x[11] = dtb;  /* a1: device tree, per the RISC-V boot protocol */
	s->mstatus = MSTATUS_MPP;  /* previous privilege is machine */
	s->mtvec = 0;
	s->mscratch = 0;
	s->mepc = 0;
	s->mcause = 0;
	s->mtval = 0;
	s->mie = 0;
	s->mip = 0;
	s->cycle_lo = s->cycle_hi = 0;
	s->time_lo = s->time_hi = 0;
	s->timecmp_lo = s->timecmp_hi = 0;
	s->waiting_for_interrupt = false;
	s->priv = 3;   /* machine */
	s->reservation = NO_RESERVATION;
	s->stop_value = 0;
	s->retired = 0;
	s->pending_char = -1;
}

/* ---- MMIO ------------------------------------------------------------- */

/* Set when a SYSCON write asks the machine to stop; rv32_run turns it into
   a stop reason.  Kept out of the hot path's return values.
   Deliberately not inlined and deliberately not in .fastcode: a device
   access is slow whatever happens, and A0 RAM is 5 KB that the decode path
   has better uses for. */
/* The device paths were deliberately kept out of internal RAM: they had been
   inlined into .fastcode, and marking them noinline freed 380 bytes and cost
   nothing.  That was measured against guest/bench.c, which touches a device
   four times in a whole run.  A Linux boot does it once in 121 instructions
   -- 0.8% of everything it executes, two thirds of everything the assembly
   path declines -- so they are hot code now, and they live in the window
   buffer where rv32_hot.s can afford to call them.  RV32_MMIO is in rv32.h,
   with the other placement switches. */

RV32_MMIO static rv32_stop_t
mmio_store(rv32_t *s, uint32_t addr, uint32_t value)
{
	if (addr == RV_UART_BASE) {
		if (s->putchar)
			s->putchar(s->console_arg, (int)(value & 0xff));
		return RV_RAN_OUT;
	}
	if (addr == RV_CLINT_BASE + 0x4000) {
		s->timecmp_lo = value;
		return RV_RAN_OUT;
	}
	if (addr == RV_CLINT_BASE + 0x4004) {
		s->timecmp_hi = value;
		return RV_RAN_OUT;
	}
	if (addr == RV_MARK) {
		if (s->mark)
			s->mark(s->console_arg, value);
		return RV_RAN_OUT;
	}
	if (addr == RV_SYSCON) {
		s->stop_value = value;
		if (value == 0x5555)
			return RV_POWEROFF;
		if (value == 0x7777)
			return RV_REBOOT;
		return RV_RAN_OUT;
	}
	/* Unclaimed device registers read as zero and swallow writes, which is
	   what a dtb-less probe expects to find. */
	return RV_RAN_OUT;
}

/* The console source is drained into a one-byte holding register so that the
   guest can poll the line-status bit without consuming input. */
RV32_MMIO static int console_peek(rv32_t *s)
{
	if (s->pending_char < 0 && s->getchar)
		s->pending_char = s->getchar(s->console_arg);
	return s->pending_char;
}

RV32_MMIO uint32_t rv32_mmio_load(rv32_t *s, uint32_t addr)
{
	if (addr == RV_UART_BASE) {
		int c = console_peek(s);
		s->pending_char = -1;
		return c < 0 ? 0 : (uint32_t)c;
	}
	if (addr == RV_UART_BASE + 5) {
		/* 8250 line status: transmitter always empty, bit 0 says a
		   received byte is waiting. */
		return 0x60 | (console_peek(s) >= 0 ? 1u : 0u);
	}
	if (addr == RV_CLINT_BASE + 0xbff8)
		return s->time_lo;
	if (addr == RV_CLINT_BASE + 0xbffc)
		return s->time_hi;
	if (addr == RV_CLINT_BASE + 0x4000)
		return s->timecmp_lo;
	if (addr == RV_CLINT_BASE + 0x4004)
		return s->timecmp_hi;
	return 0;
}

/* Carry out a device store, or say that the C interpreter has to.
 *
 * Two addresses it has to.  The marker reads the retired count, and rv32_hot
 * does not publish that until it returns; SYSCON stops the machine, which is
 * a thing only the interpreter's caller can do.  Both are once-a-run
 * addresses, so declining them costs nothing and keeps the benchmark's
 * per-kernel instruction counts honest.
 */
RV32_MMIO int rv32_mmio_store_hot(rv32_t *s, uint32_t addr, uint32_t value)
{
	if (addr == RV_MARK || addr == RV_SYSCON)
		return 1;
	mmio_store(s, addr, value);      /* cannot stop for any other address */
	return 0;
}

#ifdef RV32_JIT
/* A CSR access from translated code: the same registers the interpreter
   below knows, read and written the same way.  Returns nonzero for the one
   it cannot do -- instret, which is only exact inside the interpreter's
   batch -- and the interpreter then executes it. */
int rv32_csr_hot(rv32_t *s, uint32_t ir, uint32_t rs1, uint32_t *out)
{
	uint32_t csr = ir >> 20;
	uint32_t funct3 = (ir >> 12) & 7;
	uint32_t write = (funct3 & 4) ? ((ir >> 15) & 0x1f) : rs1;
	uint32_t old, new_value;

	switch (csr) {
	case 0x300: old = s->mstatus; break;
	case 0x301: old = 0x40001101; break;  /* misa: rv32ima */
	case 0x304: old = s->mie; break;
	case 0x305: old = s->mtvec; break;
	case 0x340: old = s->mscratch; break;
	case 0x341: old = s->mepc; break;
	case 0x342: old = s->mcause; break;
	case 0x343: old = s->mtval; break;
	case 0x344: old = s->mip; break;
	case 0xc00: case 0xb00: old = s->cycle_lo; break;
	case 0xc80: case 0xb80: old = s->cycle_hi; break;
	case 0xc01: old = s->time_lo; break;
	case 0xc81: old = s->time_hi; break;
	case 0xc02: case 0xb02: case 0xc82: case 0xb82:
		return 1;
	default: old = 0; break;
	}
	switch (funct3 & 3) {
	case 1: new_value = write; break;
	case 2: new_value = old | write; break;
	default: new_value = old & ~write; break;
	}
	if ((funct3 & 3) != 1 && ((ir >> 15) & 0x1f) == 0)
		new_value = old;
	switch (csr) {
	case 0x300: s->mstatus = new_value; break;
	case 0x304: s->mie = new_value; break;
	case 0x305: s->mtvec = new_value; break;
	case 0x340: s->mscratch = new_value; break;
	case 0x341: s->mepc = new_value; break;
	case 0x342: s->mcause = new_value; break;
	case 0x343: s->mtval = new_value; break;
	case 0x344: s->mip = new_value; break;
	default: break;
	}
	*out = old;
	return 0;
}

/* An atomic from translated code, on a host address the code has already
   checked is inside RAM and aligned.  Returns what goes in rd. */
uint32_t rv32_amo_hot(rv32_t *s, uint32_t ir, uint8_t *p, uint32_t b)
{
	uint32_t op = ir >> 27;
	uint32_t old = *(uint32_t *)p;
	uint32_t addr = RV_RAM_BASE + (uint32_t)(p - s->ram);

	if (op == 2) {           /* LR.W */
		s->reservation = addr;
		return old;
	}
	if (op == 3) {           /* SC.W */
		if (s->reservation != addr)
			return 1;
		*(uint32_t *)p = b;
		s->reservation = NO_RESERVATION;
		return 0;
	}
	switch (op) {
	case 0x00: *(uint32_t *)p = old + b; break;
	case 0x01: *(uint32_t *)p = b; break;
	case 0x04: *(uint32_t *)p = old ^ b; break;
	case 0x08: *(uint32_t *)p = old | b; break;
	case 0x0c: *(uint32_t *)p = old & b; break;
	case 0x10: *(uint32_t *)p = (int32_t)old < (int32_t)b ? b : old; break;
	case 0x14: *(uint32_t *)p = (int32_t)old > (int32_t)b ? b : old; break;
	case 0x18: *(uint32_t *)p = old < b ? b : old; break;
	default:   *(uint32_t *)p = old > b ? b : old; break;
	}
	return old;
}
#endif

#if defined(RV32_ASM) || defined(RV32_JIT)
#ifdef RV32_ASM
/* rv32_hot.s */
uint32_t rv32_hot(rv32_t *s, uint32_t budget);
#endif

/* The M-extension operations the assembly does not do itself: the divides,
   and the one mixed-sign multiply.  rv32_div.s, in the window buffer; it
   is called rather than declined to, because declining costs a round trip
   out of the hot path and back in through the whole C interpreter, and real
   code divides often enough for that to show.  The semantics are RISC-V's,
   which differ from C's on division by zero and on the one overflowing
   signed case. */
uint32_t rv32_divop(uint32_t funct3, uint32_t a, uint32_t b);
#endif

/* ---- the interpreter --------------------------------------------------- */

/* One batch of interpretation in portable C.  This is the reference
   implementation, and with an assembly hot path built in it is also the
   fallback: the assembly declines whatever it does not implement and this
   runs exactly one instruction before it resumes. */
RV32_HOT static rv32_stop_t rv32_interpret(rv32_t *s, uint32_t budget)
{
	uint32_t *x = s->x;
	uint8_t *ram = s->ram;
	const uint32_t ram_size = s->ram_size;
	uint32_t pc = s->pc;
	rv32_stop_t stop = RV_RAN_OUT;
	/* The retired count is 64 bits in the machine state, so incrementing it
	   per instruction is a read-modify-write of two words on the hot path.
	   It is brought up to date from the loop counter at every exit and
	   before any call that can observe it; `published` records how much of
	   the current batch has already been added.  Deliberately a 32-bit
	   local -- holding a 64-bit base live across the loop costs two of the
	   C33's registers and measured slower than the per-instruction
	   increment it replaced. */
	uint32_t published = 0;

	/* One entry per 7-bit opcode, so the dispatch is a shift, a load and
	   an indirect jump with nothing else around it.  A C switch cannot
	   get here: on the 7-bit opcode gcc emits two range checks, a
	   53-entry table and a compare chain, and on the dense 5-bit key it
	   still emits a range check and needs the low two bits tested
	   separately.  Encodings whose low bits are not 11 are compressed
	   instructions, which this core does not implement; they land on the
	   illegal entry like any other hole. */
	static void *dispatch[128] RV32_HOTDATA;
	if (!dispatch[0]) {
		for (unsigned i = 0; i < 128; ++i)
			dispatch[i] = &&op_illegal;
		dispatch[0x03] = &&op_load;    dispatch[0x0f] = &&op_fence;
		dispatch[0x13] = &&op_imm;     dispatch[0x17] = &&op_auipc;
		dispatch[0x23] = &&op_store;   dispatch[0x2f] = &&op_amo;
		dispatch[0x33] = &&op_reg;     dispatch[0x37] = &&op_lui;
		dispatch[0x63] = &&op_branch;  dispatch[0x67] = &&op_jalr;
		dispatch[0x6f] = &&op_jal;     dispatch[0x73] = &&op_system;
	}

	for (uint32_t n = 0; n < budget; ++n) {
		uint32_t trap = 0;
		uint32_t rval = 0;   /* value to write back to rd */
		uint32_t rdid;
		uint32_t ir;
		uint32_t next_pc = pc + 4;

		if ((pc - RV_RAM_BASE) >= ram_size) {
			trap = TRAP_EXC(CAUSE_INSN_FAULT);
			goto take_trap;
		}
		ir = *(const uint32_t *)(ram + (pc - RV_RAM_BASE));
		rdid = (ir >> 7) & 0x1f;

		goto *dispatch[ir & 0x7f];
		op_lui:  /* 0x37 */
			rval = ir & 0xfffff000u;
			goto writeback;

		op_auipc:  /* 0x17 */
			rval = pc + (ir & 0xfffff000u);
			goto writeback;

		op_jal:  /* 0x6f */
		{
			int32_t imm = ((ir >> 20) & 0x7fe) | ((ir >> 9) & 0x800) |
				(ir & 0xff000) | ((ir & 0x80000000u) ? 0xfff00000u : 0);
			rval = pc + 4;
			next_pc = pc + (uint32_t)imm;
			if (next_pc & 3) {
				trap = TRAP_EXC(CAUSE_INSN_MISALIGNED);
				s->mtval = next_pc;
				goto take_trap;
			}
			goto writeback;
		}

		op_jalr:  /* 0x67 */
		{
			int32_t imm = (int32_t)ir >> 20;
			rval = pc + 4;
			next_pc = (x[(ir >> 15) & 0x1f] + (uint32_t)imm) & ~1u;
			if (next_pc & 3) {
				trap = TRAP_EXC(CAUSE_INSN_MISALIGNED);
				s->mtval = next_pc;
				goto take_trap;
			}
			goto writeback;
		}

		op_branch:  /* 0x63 */
		{
			uint32_t a = x[(ir >> 15) & 0x1f];
			uint32_t b = x[(ir >> 20) & 0x1f];
			int32_t imm = ((ir >> 7) & 0x1e) | ((ir >> 20) & 0x7e0) |
				((ir << 4) & 0x800) | ((ir & 0x80000000u) ? 0xfffff000u : 0);
			bool taken;
			switch ((ir >> 12) & 7) {
			case 0: taken = a == b; break;
			case 1: taken = a != b; break;
			case 4: taken = (int32_t)a < (int32_t)b; break;
			case 5: taken = (int32_t)a >= (int32_t)b; break;
			case 6: taken = a < b; break;
			case 7: taken = a >= b; break;
			default: trap = TRAP_EXC(CAUSE_ILLEGAL_INSN); taken = false; break;
			}
			if (trap)
				goto take_trap;
			if (taken) {
				next_pc = pc + (uint32_t)imm;
				if (next_pc & 3) {
					trap = TRAP_EXC(CAUSE_INSN_MISALIGNED);
					s->mtval = next_pc;
					goto take_trap;
				}
			}
			rdid = 0;
			goto writeback;
		}

		op_load:  /* 0x03 */
		{
			uint32_t addr = x[(ir >> 15) & 0x1f] + (uint32_t)((int32_t)ir >> 20);
			uint32_t off = addr - RV_RAM_BASE;
			uint32_t width = (ir >> 12) & 7;
			if (width > 5 || width == 3) {
				trap = TRAP_EXC(CAUSE_ILLEGAL_INSN);
				goto take_trap;
			}
			/* Alignment mask from the width: 0/4 -> 0, 1/5 -> 1,
			   2 -> 3.  A table would be one more SDRAM read on
			   every guest load. */
			if (addr & ((1u << (width & 3)) - 1)) {
				trap = TRAP_EXC(CAUSE_LOAD_MISALIGNED);
				s->mtval = addr;
				goto take_trap;
			}
			if (off >= ram_size) {
				rval = rv32_mmio_load(s, addr);
				goto writeback;
			}
			switch (width) {
			case 0: rval = (uint32_t)(int32_t)(int8_t)ram[off]; break;
			case 1: rval = (uint32_t)(int32_t)*(const int16_t *)(ram + off); break;
			case 2: rval = *(const uint32_t *)(ram + off); break;
			case 4: rval = ram[off]; break;
			case 5: rval = *(const uint16_t *)(ram + off); break;
			}
			goto writeback;
		}

		op_store:  /* 0x23 */
		{
			uint32_t addr = x[(ir >> 15) & 0x1f] +
				(uint32_t)(((int32_t)(ir & 0xfe000000u) >> 20) | ((ir >> 7) & 0x1f));
			uint32_t value = x[(ir >> 20) & 0x1f];
			uint32_t off = addr - RV_RAM_BASE;
			uint32_t width = (ir >> 12) & 7;
			rdid = 0;
			if (width > 2) {
				trap = TRAP_EXC(CAUSE_ILLEGAL_INSN);
				goto take_trap;
			}
			if (addr & ((1u << width) - 1)) {
				trap = TRAP_EXC(CAUSE_STORE_MISALIGNED);
				s->mtval = addr;
				goto take_trap;
			}
			if (off >= ram_size) {
				/* The benchmark marker reads the retired count
				   from inside this call, so publish it first. */
				s->retired += n - published;
				published = n;
				stop = mmio_store(s, addr, value);
				if (stop != RV_RAN_OUT) {
					s->retired += 1;
					s->cycle_lo += n + 1;
					s->pc = next_pc;
					return stop;
				}
				goto writeback;
			}
			switch (width) {
			case 0: ram[off] = (uint8_t)value; break;
			case 1: *(uint16_t *)(ram + off) = (uint16_t)value; break;
			case 2: *(uint32_t *)(ram + off) = value; break;
			}
			/* A store into a reserved word breaks the reservation. */
			if ((addr & ~3u) == s->reservation)
				s->reservation = NO_RESERVATION;
			goto writeback;
		}

		/* OP-IMM and OP are the two commonest opcodes and they are
		   separate table entries, so they get separate bodies: sharing
		   one costs an `is_reg` test in the operand fetch and again in
		   the add/subtract, on every arithmetic instruction. */
		op_imm:  /* 0x13 */
		{
			uint32_t a = x[(ir >> 15) & 0x1f];
			uint32_t b = (uint32_t)((int32_t)ir >> 20);
			switch ((ir >> 12) & 7) {
			case 0: rval = a + b; break;
			case 1: rval = a << (b & 31); break;
			case 2: rval = (int32_t)a < (int32_t)b; break;
			case 3: rval = a < b; break;
			case 4: rval = a ^ b; break;
			case 5:
				/* The shift amount is the low five bits, so the
				   arithmetic bit survives the immediate. */
				rval = (ir & 0x40000000u)
					? (uint32_t)((int32_t)a >> (b & 31))
					: (a >> (b & 31));
				break;
			case 6: rval = a | b; break;
			case 7: rval = a & b; break;
			}
			goto writeback;
		}

		op_reg:  /* 0x33 */
		{
			uint32_t a = x[(ir >> 15) & 0x1f];
			uint32_t b = x[(ir >> 20) & 0x1f];
			uint32_t funct3 = (ir >> 12) & 7;

			if (ir & 0x02000000u) {  /* M extension */
				switch (funct3) {
				case 0: rval = a * b; break;
				case 1: rval = (uint32_t)(((int64_t)(int32_t)a * (int64_t)(int32_t)b) >> 32); break;
				case 2: rval = (uint32_t)(((int64_t)(int32_t)a * (int64_t)(uint64_t)b) >> 32); break;
				case 3: rval = (uint32_t)(((uint64_t)a * (uint64_t)b) >> 32); break;
				case 4:  /* DIV */
					if (b == 0)
						rval = 0xffffffffu;
					else if (a == 0x80000000u && b == 0xffffffffu)
						rval = a;
					else
						rval = (uint32_t)((int32_t)a / (int32_t)b);
					break;
				case 5:  /* DIVU */
					rval = b == 0 ? 0xffffffffu : a / b;
					break;
				case 6:  /* REM */
					if (b == 0)
						rval = a;
					else if (a == 0x80000000u && b == 0xffffffffu)
						rval = 0;
					else
						rval = (uint32_t)((int32_t)a % (int32_t)b);
					break;
				case 7:  /* REMU */
					rval = b == 0 ? a : a % b;
					break;
				}
				goto writeback;
			}

			switch (funct3) {
			case 0: rval = (ir & 0x40000000u) ? a - b : a + b; break;
			case 1: rval = a << (b & 31); break;
			case 2: rval = (int32_t)a < (int32_t)b; break;
			case 3: rval = a < b; break;
			case 4: rval = a ^ b; break;
			case 5:
				rval = (ir & 0x40000000u)
					? (uint32_t)((int32_t)a >> (b & 31))
					: (a >> (b & 31));
				break;
			case 6: rval = a | b; break;
			case 7: rval = a & b; break;
			}
			goto writeback;
		}

		op_fence:  /* 0x0f */
#ifdef RV32_JIT
			/* fence.i: code may have changed under a translation. */
			if (((ir >> 12) & 7) == 1 && rv32_jit.code)
				rv32_jit_fence(s);
#endif
			rdid = 0;
			goto writeback;

		op_system:  /* 0x73 */
		{
			uint32_t csr = ir >> 20;
			uint32_t funct3 = (ir >> 12) & 7;
			if (funct3 & 3) {  /* CSRRW/S/C and their immediate forms */
				uint32_t write = (funct3 & 4) ? ((ir >> 15) & 0x1f)
							      : x[(ir >> 15) & 0x1f];
				uint32_t old;
				switch (csr) {
				case 0x300: old = s->mstatus; break;
				case 0x301: old = 0x40001101; break;  /* misa: rv32ima */
				case 0x304: old = s->mie; break;
				case 0x305: old = s->mtvec; break;
				case 0x340: old = s->mscratch; break;
				case 0x341: old = s->mepc; break;
				case 0x342: old = s->mcause; break;
				case 0x343: old = s->mtval; break;
				case 0x344: old = s->mip; break;
				case 0xc00: case 0xb00: old = s->cycle_lo; break;
				case 0xc80: case 0xb80: old = s->cycle_hi; break;
				case 0xc01: old = s->time_lo; break;
				case 0xc81: old = s->time_hi; break;
				case 0xc02: case 0xb02:   /* instret */
					old = (uint32_t)(s->retired + (n - published)); break;
				case 0xc82: case 0xb82:
					old = (uint32_t)((s->retired + (n - published)) >> 32); break;
				case 0xf11: case 0xf12: case 0xf13: case 0xf14:
					old = 0;  /* vendor/arch/impl/hart ids */
					break;
				default: old = 0; break;
				}
				uint32_t new_value;
				switch (funct3 & 3) {
				case 1: new_value = write; break;
				case 2: new_value = old | write; break;
				default: new_value = old & ~write; break;
				}
				/* rs1 == x0 on a set/clear means "read only". */
				if ((funct3 & 3) != 1 && ((ir >> 15) & 0x1f) == 0)
					new_value = old;
				switch (csr) {
				case 0x300: s->mstatus = new_value; break;
				case 0x304: s->mie = new_value; break;
				case 0x305: s->mtvec = new_value; break;
				case 0x340: s->mscratch = new_value; break;
				case 0x341: s->mepc = new_value; break;
				case 0x342: s->mcause = new_value; break;
				case 0x343: s->mtval = new_value; break;
				case 0x344: s->mip = new_value; break;
				default: break;
				}
				rval = old;
				goto writeback;
			}
			if (funct3 == 0) {
				if (csr == 0x000) {        /* ECALL */
					trap = TRAP_EXC(s->priv ? CAUSE_ECALL_M
								: CAUSE_ECALL_U);
					goto take_trap;
				}
				if (csr == 0x001) {        /* EBREAK */
					trap = TRAP_EXC(CAUSE_BREAKPOINT);
					goto take_trap;
				}
				if (csr == 0x302) {        /* MRET */
					uint32_t was = s->mstatus;
					s->mstatus = ((was & MSTATUS_MPIE) >> 4) |
						(s->priv << 11) | MSTATUS_MPIE;
					s->priv = (was >> 11) & 3;
					next_pc = s->mepc;
					if (next_pc & 3) {
						trap = TRAP_EXC(CAUSE_INSN_MISALIGNED);
						s->mtval = next_pc;
						goto take_trap;
					}
					rdid = 0;
					goto writeback;
				}
				if (csr == 0x105) {        /* WFI */
					s->waiting_for_interrupt = true;
					s->pc = next_pc;
					s->retired += n + 1 - published;
					s->cycle_lo += n + 1;
					return RV_RAN_OUT;
				}
				if ((csr & 0xfe0) == 0x120) {  /* SFENCE.VMA: no MMU */
					rdid = 0;
					goto writeback;
				}
			}
			trap = TRAP_EXC(CAUSE_ILLEGAL_INSN);
			goto take_trap;
		}

		op_amo:  /* 0x2f */
		{
			uint32_t addr = x[(ir >> 15) & 0x1f];
			uint32_t off = addr - RV_RAM_BASE;
			uint32_t op = ir >> 27;
			uint32_t b = x[(ir >> 20) & 0x1f];
			uint32_t old;
			if (((ir >> 12) & 7) != 2) {   /* 64-bit AMOs are not rv32 */
				trap = TRAP_EXC(CAUSE_ILLEGAL_INSN);
				goto take_trap;
			}
			if (addr & 3) {
				trap = TRAP_EXC(CAUSE_STORE_MISALIGNED);
				s->mtval = addr;
				goto take_trap;
			}
			if (off >= ram_size) {
				trap = TRAP_EXC(CAUSE_STORE_FAULT);
				s->mtval = addr;
				goto take_trap;
			}
			old = *(const uint32_t *)(ram + off);
			if (op == 2) {           /* LR.W */
				s->reservation = addr;
				rval = old;
				goto writeback;
			}
			if (op == 3) {           /* SC.W */
				if (s->reservation != addr) {
					rval = 1;    /* failed */
					goto writeback;
				}
				*(uint32_t *)(ram + off) = b;
				s->reservation = NO_RESERVATION;
				rval = 0;
				goto writeback;
			}
			switch (op) {
			case 0x00: *(uint32_t *)(ram + off) = old + b; break;   /* AMOADD */
			case 0x01: *(uint32_t *)(ram + off) = b; break;         /* AMOSWAP */
			case 0x04: *(uint32_t *)(ram + off) = old ^ b; break;   /* AMOXOR */
			case 0x08: *(uint32_t *)(ram + off) = old | b; break;   /* AMOOR */
			case 0x0c: *(uint32_t *)(ram + off) = old & b; break;   /* AMOAND */
			case 0x10: *(uint32_t *)(ram + off) =
				(int32_t)old < (int32_t)b ? b : old; break;     /* AMOMIN */
			case 0x14: *(uint32_t *)(ram + off) =
				(int32_t)old > (int32_t)b ? b : old; break;     /* AMOMAX */
			case 0x18: *(uint32_t *)(ram + off) = old < b ? b : old; break;
			case 0x1c: *(uint32_t *)(ram + off) = old > b ? b : old; break;
			default:
				trap = TRAP_EXC(CAUSE_ILLEGAL_INSN);
				goto take_trap;
			}
			rval = old;
			goto writeback;
		}

		op_illegal:
			trap = TRAP_EXC(CAUSE_ILLEGAL_INSN);
			goto take_trap;

	writeback:
		if (rdid)
			x[rdid] = rval;
		pc = next_pc;
		continue;

	take_trap:
		if (!TRAP_IS_INTERRUPT(trap)) {
			uint32_t cause = trap - 1;
			if (s->mtvec == 0) {
				/* Nothing installed: a bare-metal image has gone
				   wrong and the harness wants to see where. */
				s->pc = pc;
				s->mcause = cause;
				s->retired += n - published;
				s->cycle_lo += n;
				return RV_FAULT;
			}
			s->mepc = pc;
			s->mcause = cause;
			if (cause != CAUSE_LOAD_MISALIGNED &&
			    cause != CAUSE_STORE_MISALIGNED &&
			    cause != CAUSE_INSN_MISALIGNED)
				s->mtval = (cause == CAUSE_ILLEGAL_INSN) ? ir : pc;
			s->mstatus = ((s->mstatus & MSTATUS_MIE) << 4) |
				(s->priv << 11);
			s->priv = 3;
			pc = s->mtvec & ~3u;
		}
	}

	s->retired += budget - published;
	s->cycle_lo += budget;
	s->pc = pc;
	(void)stop;
	return RV_RAN_OUT;
}


rv32_stop_t rv32_run(rv32_t *s, uint32_t budget, uint32_t time_ticks)
{
	/* Advance the guest clock once per batch rather than per instruction:
	   the guest cannot tell the difference at this granularity and the
	   64-bit add is expensive on a 32-bit machine. */
	uint32_t t = s->time_lo + time_ticks;
	if (t < s->time_lo)
		s->time_hi++;
	s->time_lo = t;

	if ((s->timecmp_hi || s->timecmp_lo) &&
	    (s->time_hi > s->timecmp_hi ||
	     (s->time_hi == s->timecmp_hi && s->time_lo >= s->timecmp_lo))) {
		s->waiting_for_interrupt = false;
		s->mip |= MIP_MTIP;
	} else {
		s->mip &= ~MIP_MTIP;
	}

	if (s->waiting_for_interrupt) {
		s->cycle_lo += budget;   /* WFI still burns guest cycles */
		return RV_RAN_OUT;
	}

	/* Take a pending timer interrupt at a batch boundary. */
	if ((s->mip & MIP_MTIP) && (s->mie & MIE_MTIE) && (s->mstatus & MSTATUS_MIE)) {
		s->mepc = s->pc;
		s->mcause = CAUSE_MACHINE_TIMER;
		s->mtval = 0;
		s->mstatus = ((s->mstatus & MSTATUS_MIE) << 4) |
			(s->priv << 11);
		s->priv = 3;
		s->pc = s->mtvec & ~3u;
	}

#ifdef RV32_JIT
	/* Translated code runs until it meets something it was not translated
	   for, and hands back the guest pc of exactly that instruction.  What
	   is not translated -- because it is not worth translating yet, or
	   because this cannot translate it at all -- the assembly hot path
	   interprets, in chunks, so that code which turns out to be hot is
	   noticed without asking after every instruction.  Progress is
	   guaranteed the same way it always was: the C fallback consumes one. */
	if (rv32_jit.code) {
		while (budget) {
			uint8_t *code;
			uint32_t did;

			uint32_t t0 = rv32_jit.clock ? rv32_jit.clock() : 0;

			code = rv32_jit.step ? NULL : rv32_jit_block(s, s->pc);
			if (rv32_jit.clock) {
				uint32_t t1 = rv32_jit.clock();

				rv32_jit.cyc_translate += (uint32_t)(t1 - t0);
				t0 = t1;
			}
			if (code) {
				rv32_jit.entries++;
				did = rv32_jit_enter(s, budget, code);
				if (rv32_jit.clock)
					rv32_jit.cyc_cache +=
						(uint32_t)(rv32_jit.clock() - t0);
				s->retired += did;
				s->cycle_lo += did;
				/* A block that has started runs whole, so this
				   can overrun the batch by one block's worth.  A
				   loop laid out to fit the fetch window charges
				   before it checks and refuses a pass the batch
				   cannot afford: what is left of the batch then is
				   the interpreter's, or this would ask again. */
				budget = did >= budget ? 0 : budget - did;
				if (!did)
					rv32_jit.step = 1;
				continue;
			}
			rv32_jit.step = 0;
			{
				uint32_t want = budget < RV32_JIT_CHUNK
					? budget : RV32_JIT_CHUNK;

				did = rv32_hot(s, want);
				if (rv32_jit.clock)
					rv32_jit.cyc_interpret +=
						(uint32_t)(rv32_jit.clock() - t0);
				s->retired += did;
				s->cycle_lo += did;
				budget -= did;
				if (did == want)
					continue;
			}
			{
				rv32_stop_t stop = rv32_interpret(s, 1);

				--budget;
				if (stop != RV_RAN_OUT)
					return stop;
			}
		}
		return RV_RAN_OUT;
	}
	return rv32_interpret(s, budget);
#elif defined(RV32_ASM)
	/* The assembly runs until it meets something it does not implement;
	   the C interpreter then executes exactly that one instruction and
	   the assembly picks up again.  Progress is guaranteed because the
	   fallback always consumes one. */
	while (budget) {
		uint32_t did = rv32_hot(s, budget);
		s->retired += did;
		s->cycle_lo += did;
		budget -= did;
		if (!budget)
			break;
		rv32_stop_t stop = rv32_interpret(s, 1);
		--budget;
		if (stop != RV_RAN_OUT)
			return stop;
	}
	return RV_RAN_OUT;
#else
	return rv32_interpret(s, budget);
#endif
}
