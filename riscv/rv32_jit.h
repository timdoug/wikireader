/* rv32_jit.h - translated rv32 code, and the state the runtime shares.
 *
 * The interpreter spends about two thirds of every guest instruction taking
 * the instruction word apart and dispatching on it -- work that depends only
 * on the word and not on the machine, and that a translator does once instead
 * of every time.  jit_probe.s asked the hardware what the other end costs and
 * README.md has the answers; this is the translator those measurements
 * describe.
 *
 * A translated block is entered with the same registers the assembly hot path
 * keeps live, for the same reasons:
 *
 *   %r0   s, which is also the guest register file: x[n] is at [%r0 + n*4]
 *   %r2   ram, the host base of guest RAM
 *   %r3   ram_end
 *   %r6   instructions left in the batch, counted down a block at a time
 *   %r8   ram - RV_RAM_BASE, so a guest address converts with one add
 *   %r9   the page map a store checks itself against
 *   %r4, %r5, %r13, %r14  scratch
 *
 * %r1, %r7, %r10, %r11 and %r12 are free, which is what a register allocator
 * would have to work with.  %r15 is not: it is the global data pointer.
 */

#ifndef RV32_JIT_H
#define RV32_JIT_H

#include "rv32.h"

/* Byte offsets rv32_jit.s depends on.  rv32_jit.c asserts every one of them
   against the struct, so the two cannot drift apart silently. */
#define RV32_JOFF_MAP      0
#define RV32_JOFF_PAGE     4
#define RV32_JOFF_BACK     8

/* The map the runtime probes on an indirect jump: a direct-mapped cache of
   guest pc to translated code, two words a slot, indexed by (pc >> 2).  It is
   a cache and not an index -- a collision throws the older entry away and the
   block it named is found again through C, which is slower and still right.

   Slower by a lot, though: finding it through C means leaving the code cache,
   and a Linux boot translates seventeen thousand blocks.  At four thousand
   slots it left four million times in one boot; this is a quarter of a
   megabyte and the boot's block heads fit in it with room over. */
#define RV32_JIT_SLOTS 32768
#define RV32_JIT_MASK  (RV32_JIT_SLOTS - 1)

/* Translating is expensive -- about 1,400 C33 instructions for one guest
   instruction, which from SDRAM is some 6,800 cycles -- and a Linux boot
   executes 385 KiB of guest code, most of it once.  Translating everything
   made the boot *slower* than the interpreter: 99.6 cycles a guest instruction
   against 85.3.  So a stretch of guest code has to earn its translation by
   being run, and until it has, rv32_hot.s interprets it at about the same
   speed the interpreter always managed.
   
   Hotness is counted by 256-byte region of guest code rather than by block,
   because the interpreter stops wherever its batch runs out and not at a block
   head: a loop keeps landing in the same region however its iterations are
   cut up, and a straight run through cold code touches each region once. */
#define RV32_JIT_HOT_SLOTS 8192
#define RV32_JIT_HOT_MASK  (RV32_JIT_HOT_SLOTS - 1)
#ifndef RV32_JIT_HOT
#define RV32_JIT_HOT 12
#endif
/* Guest instructions the interpreter runs between chances to notice that the
   code it is in has become worth translating. */
#define RV32_JIT_CHUNK 256

/* Why a translated block gave up.  The stub that carries each one is named in
   the code cache by its address, so these are the argument the runtime hands
   rv32_jit_fault(). */
enum {
	RV32_JIT_DECLINE = 0,  /* an instruction, or an address, this cannot do */
	RV32_JIT_STORE   = 1,  /* a store into a page the map marks */
	RV32_JIT_BUDGET  = 2,  /* the batch is spent; s->pc is the block head */
};

typedef struct rv32_jit {
	/* Shared with the assembly; see RV32_JOFF_* above. */
	uint32_t *map;
	/* One byte per 256 bytes of guest RAM: bit 0 says it holds code that
	   has been translated, bit 1 that a load reserved a word in it.  A
	   store into either is the runtime's business and every other store --
	   which is nearly all of them -- reads a zero and carries on.
	   
	   256 bytes because 4 KB was too coarse to tell a program's text from
	   the data right after it: rvbench writes its bss, which shares a page
	   with its code, and flushed the whole cache thirty-four times in one
	   run of the benchmark.
	   
	   This was a single span from the lowest guest address with code in it
	   to the highest, which is two compares instead of a load.  By the time
	   a Linux boot reaches userspace that span is seven megabytes and holds
	   the kernel's data as well as its text: two hundred thousand ordinary
	   stores a boot left the code cache to be told they were ordinary. */
	uint8_t  *page;
	/* Instructions the block charged the batch for and did not execute.
	   A block charges for all of itself before it starts, so that a batch
	   which overruns is still counted exactly; when it gives up half way
	   the rest has to be given back, or `retired` is a number no benchmark
	   and no guest clock can trust. */
	uint32_t  back;

	/* C's alone from here down. */
	uint8_t  *code;        /* the code cache */
	uint32_t  code_size;
	uint32_t  code_used;

	struct jit_block *blk; /* one a translated block, in code order */
	uint32_t  blk_max;
	uint32_t  blk_used;
	uint16_t *blk_hash;    /* guest pc -> block index, open addressed */
	uint32_t  blk_hash_mask;
	uint8_t  *hot;         /* how often each region has been interpreted */
	uint32_t  page_max;
	uint32_t  held;        /* where bit 1 is set, or ~0 */
	uint8_t  *mark;        /* bytes of code a guest instruction became */
	uint32_t  mark_max;
	uint32_t  mark_used;

	struct jit_link *link; /* exits waiting for their target to exist */
	uint32_t  link_max;
	uint32_t  link_used;
	uint16_t *link_hash;
	uint32_t  link_hash_mask;

	int       step;        /* the instruction at s->pc is C's to execute */
	uint32_t  code_lo;     /* guest addresses the cache holds code for */
	uint32_t  code_hi;

	uint32_t  flushes;     /* what the run cost, for the report */
	uint32_t  blocks;
	uint32_t  bytes;
	uint32_t  declines;
	uint32_t  dec_mem;     /* ...of them, a device address or a bad width */
	uint32_t  dec_dev;
	uint32_t  dec_align;
	uint32_t  dec_jump;    /* ...a target outside RAM or off a word */
	uint32_t  stores;
	uint32_t  entries;
	uint32_t  warmups;     /* chunks the interpreter ran instead */
} rv32_jit_t;

extern rv32_jit_t rv32_jit;

/* Carve the arena into a code cache and its tables.  Returns 0 if what it was
   given is too small to be worth using, in which case the caller should fall
   back to the interpreter. */
int rv32_jit_init(void *arena, uint32_t bytes, uint32_t ram_size);

/* The translated code for a guest pc, translating it if need be.  NULL when
   the instruction there is one this cannot translate -- the caller then has
   the C interpreter execute exactly that one, as the assembly path does. */
uint8_t *rv32_jit_block(rv32_t *s, uint32_t pc);

/* Called by the runtime stubs when a block gives up.  Returns the guest pc to
   resume at; `addr` is the host address of the store for RV32_JIT_STORE and
   is ignored otherwise. */
uint32_t rv32_jit_fault(rv32_t *s, uint32_t ra, uint32_t kind, uint32_t addr);

/* Bring the page map up to date after C has run an instruction: a load
   reserved marks the page it reserved in, a store conditional unmarks it. */
void rv32_jit_reserve(rv32_t *s);

/* Throw the cache away.  Safe only from outside it. */
void rv32_jit_flush(void);

/* rv32_jit.s */
uint32_t rv32_jit_enter(rv32_t *s, uint32_t budget, void *code);

#endif
