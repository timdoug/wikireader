/*
 * Epson C33 (S1C33) CPU emulator.
 *
 * Fixed 16-bit instructions. Wider immediates are built by up to two `ext`
 * prefix instructions, each carrying 13 bits:
 *
 *     imm = (ext1 << (W + 13)) | (ext2 << W) | base_field
 *
 * where W is the base instruction's own immediate width and ext1 is the
 * first prefix encountered (most significant). Verified against real
 * firmware: ext 0x23f; ext 0x1fff; ld.w %r15,0x3c -> 0x11fffffc.
 */

#ifndef C33_H
#define C33_H

#include <stdint.h>
#include <stdbool.h>

/* Special register file, indices as encoded in ld.w %sreg,%rN (0xa00N). */
enum c33_sreg {
	SR_PSR = 0, SR_SP = 1, SR_ALR = 2, SR_AHR = 3,
	SR_LCO = 4, SR_LSA = 5, SR_LEA = 6, SR_SOR = 7,
	SR_TTBR = 8, SR_DP = 9, SR_IDIR = 10, SR_DBBR = 11,
	SR_RSVD12 = 12, SR_USP = 13, SR_SSP = 14, SR_PC = 15,
	SR_COUNT = 16
};

/*
 * PSR flags, as documented in the C33 PE Core manual's psrset entry: the
 * imm5 bit number takes "values 0, 1, 2, 3, and 4 representing bits
 * 0 (N), 1 (Z), 2 (V), 3 (C), and 4 (IE)".
 */
#define PSR_N  (1u << 0)
#define PSR_Z  (1u << 1)
#define PSR_V  (1u << 2)
#define PSR_C  (1u << 3)
#define PSR_IE (1u << 4)

struct c33;

/* Bus callbacks; the memory map lives outside the core. */
struct c33_bus {
	uint32_t (*read)(void *ctx, uint32_t addr, unsigned size);
	void     (*write)(void *ctx, uint32_t addr, unsigned size, uint32_t val);
	void     *ctx;
};

struct c33 {
	uint32_t r[16];
	uint32_t sr[SR_COUNT];
	uint32_t pc;

	/* ext prefix accumulator */
	uint32_t ext[2];
	unsigned n_ext;

	/* pending delay-slot branch (.d instruction variants) */
	bool     delay_pending;
	uint32_t delay_target;

	struct c33_bus bus;

	/* diagnostics: how often each form silently discarded ext prefixes */
	uint32_t ext_dropped[256];
	/* log each grifo syscall (int 1) by name as it is issued */
	bool     trace_syscalls;
	unsigned long syscalls;
	/* where the in-flight syscall resumes, so its result can be logged */
	uint32_t sysret_pc;
	unsigned sysret_num;

	/* pending hardware interrupt, delivered once PSR.IE allows it */
	bool     irq_pending;
	unsigned irq_vector;
	unsigned long irqs_taken;
	uint32_t cur_pc;   /* address of the instruction being executed */
	uint64_t cycles;
	bool     halted;
	const char *fault;   /* non-NULL once the CPU has faulted */
	uint32_t fault_pc;
};

void     c33_reset(struct c33 *c, uint32_t entry);
/* Request a hardware interrupt; taken when PSR.IE is set. */
void     c33_raise_irq(struct c33 *c, unsigned vector);
/* Execute one instruction (an ext prefix counts as one). */
void     c33_step(struct c33 *c);
/* Human-readable single-instruction disassembly, for tracing. */
#include <stdio.h>
void     c33_report_dropped_ext(const struct c33 *c, FILE *out);
void     c33_disasm(const struct c33 *c, uint32_t addr, char *buf, size_t len);

static inline uint32_t c33_psr(const struct c33 *c) { return c->sr[SR_PSR]; }

#endif /* C33_H */
