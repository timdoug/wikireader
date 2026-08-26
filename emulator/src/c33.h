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
/* IL[3:0], bits 11-8: the interrupt level currently being serviced. */
#define PSR_IL_SHIFT 8
#define PSR_IL_MASK  (0xfu << PSR_IL_SHIFT)

struct c33;

/* Bus callbacks; the memory map lives outside the core. */
struct c33_bus {
	uint32_t (*read)(void *ctx, uint32_t addr, unsigned size);
	void     (*write)(void *ctx, uint32_t addr, unsigned size, uint32_t val);
	/*
	 * Optional: direct host pointer for a mapped address, with the bounds
	 * of its region. Lets the fetch path skip the indirect call and the
	 * region search while the PC stays inside one region, which is nearly
	 * always. Leave NULL and everything still works, just slower.
	 */
	uint8_t *(*region)(void *ctx, uint32_t addr, uint32_t *base, uint32_t *len);
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

	/* cached fetch region: [fetch_lo, fetch_hi) maps to fetch_ptr */
	uint8_t *fetch_ptr;
	uint32_t fetch_lo, fetch_hi;
	/* same, for data reads; kept separate since code and data differ */
	uint8_t *data_ptr;
	uint32_t data_lo, data_hi;
	/* last instruction word fetched, for the runaway detector */
	uint16_t last_insn;

	/* diagnostics: how often each form silently discarded ext prefixes */
	uint32_t ext_dropped[256];
	unsigned long syscalls;
	/* where the in-flight syscall resumes, so its result can be logged */
	uint32_t sysret_pc;
	unsigned sysret_num;

	/* pending hardware interrupt, delivered once PSR.IE allows it */
	unsigned long misaligned_hits;
	bool     sleeping;           /* in HALT, waiting for an interrupt */
	unsigned long sleep_cycles;
	bool     irq_pending;
	unsigned irq_vector;
	unsigned irq_priority;
	unsigned long irqs_masked;
	unsigned long opcount[256];
	unsigned long irqs_taken;
	uint32_t cur_pc;   /* address of the instruction being executed */
	uint64_t cycles;      /* instructions retired */
	uint64_t clk;         /* MCLK cycles, per the manual's CLK lines */
	bool     halted;
	const char *fault;   /* non-NULL once the CPU has faulted */
	uint32_t fault_pc;

	/*
	 * Everything below this point is host-side wiring, not machine state:
	 * where the bus goes, which diagnostics are on, and the buffers they
	 * write into. c33_reset clears the fields above and leaves these
	 * alone, so powering the device off and on again does not unplug the
	 * emulator from itself.
	 *
	 * Add new fields on the correct side of the barrier: above it to have
	 * reset clear them, below it to have them survive.
	 */
	char     reset_barrier__[0];

	struct c33_bus bus;
	/* Optional: asks the interrupt controller whether a cause is still
	   enabled, so a request cancelled before it is taken is dropped. */
	bool   (*irq_enabled)(void *ctx, unsigned vector);
	void    *irq_ctx;
	/* log each grifo syscall (int 1) by name as it is issued */
	bool     trace_syscalls;
	bool     check_alignment;   /* raise vector 6 on misaligned access */
	bool     profile;            /* count executed instructions per opcode */
	/* Executed instructions per 1K of address space, for finding hot code. */
	bool     pc_profile;
	unsigned long *pcbuckets;
	uint32_t      *pcsample;     /* a real PC seen in each bucket */
};

void     c33_reset(struct c33 *c, uint32_t entry);
/* Request a hardware interrupt; taken when PSR.IE is set. */
void     c33_raise_irq(struct c33 *c, unsigned vector, unsigned priority);
/* Print the executed-opcode histogram gathered under c->profile. */
void     c33_dump_profile(const struct c33 *c, FILE *out);
/*
 * One bucket per instruction slot, over 2 MB of address space -- enough to
 * span the kernel at 0x10000000 and the application at 0x10040000 without
 * aliasing.  64-byte buckets were too coarse to attribute anything on this
 * target: mini-libc's memchr, delay_us and delay_loop are about 30 bytes
 * each and sit next to each other, so a single bucket covered all three and
 * the profile named whichever came first.  That is how a busy-wait in the
 * SD driver came out looking like memchr in one build and delay_us in the
 * other.  12 MB of host memory is a small price for a profile you can
 * attribute to a function and believe.
 */
#define C33_PCBUCKET_SHIFT 1
#define C33_PCBUCKETS      (1u << 20)     /* 2-byte buckets, 2 MB span */
void     c33_dump_pcprofile(const struct c33 *c, FILE *out);
/* Every non-empty bucket as "address count", for diffing two runs offline.
   The top-12 summary answers "what is hot"; this answers "what changed",
   which is the question when comparing two compilers on the same source. */
void     c33_dump_pcprofile_full(const struct c33 *c, FILE *out);
/* Execute one instruction (an ext prefix counts as one). */
void     c33_step(struct c33 *c);
/* Human-readable single-instruction disassembly, for tracing. */
#include <stdio.h>
void     c33_report_dropped_ext(const struct c33 *c, FILE *out);
void     c33_disasm(const struct c33 *c, uint32_t addr, char *buf, size_t len);

static inline uint32_t c33_psr(const struct c33 *c) { return c->sr[SR_PSR]; }

#endif /* C33_H */
