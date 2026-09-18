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
 *   %r3   ram_size / 4, which is what an access's rotated offset is checked against
 *   %r6   instructions left in the batch, counted down a block at a time
 *   %r8   ram - RV_RAM_BASE, so a guest address converts with one add
 *   %r4, %r5, %r13  scratch
 *
 * and seven more that hold guest registers for the length of a region:
 *
 *   %r1, %r7, %r9, %r10, %r11, %r12, %r14   the allocated guest registers
 *
 * Which guest register is in which is the region's *map*, fixed when it is
 * translated and recorded with each of its blocks.  Every register in the map
 * is loaded at a block's cold entry and holds the true value of its guest
 * register from then on, which is what lets a block give up anywhere: the
 * runtime stores those seven and C writes them back to x[] by the map.  %r15
 * is the global data pointer and is not free.
 */

#ifndef RV32_JIT_H
#define RV32_JIT_H

#include "rv32.h"

/* Byte offsets rv32_jitrt.s depends on.  rv32_jit.c asserts every one of them
   against the struct, so the two cannot drift apart silently. */
#define RV32_JOFF_MAP      0
#define RV32_JOFF_BACK     4
#define RV32_JOFF_SPILL    8
#define RV32_JOFF_CSRVAL  36

/* Host registers a block may hold guest registers in, and the order the
   runtime spills them in. */
#define RV32_JIT_NSLOT 7

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

/* Translating is expensive and a Linux boot executes 385 KiB of guest code,
   most of it once.  Translating everything made the boot *slower* than the
   interpreter.  So a stretch of guest code has to earn its translation by
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

/* Why a translated block gave up.  The code cache names the reason, the block
   and the instruction in one word it hands the runtime, and the runtime hands
   it on to rv32_jit_fault(). */
enum {
	RV32_JIT_DECLINE = 0,  /* an instruction, or an address, this cannot do */
	RV32_JIT_BUDGET  = 1,  /* the batch is spent; nothing was executed */
};

typedef struct rv32_jit {
	/* Shared with the assembly; see RV32_JOFF_* above. */
	uint32_t *map;
	/* Instructions the block charged the batch for and did not execute.
	   A block charges for all of itself before it starts, so that a batch
	   which overruns is still counted exactly; when it gives up half way
	   the rest has to be given back, or `retired` is a number no benchmark
	   and no guest clock can trust. */
	uint32_t  back;
	/* The seven allocated registers, as they were when a block gave up.
	   rv32_jit_fault() writes them back to x[] by the block's map. */
	uint32_t  spill[RV32_JIT_NSLOT];
	/* What a CSR access read, for the runtime to hand the block. */
	uint32_t  csrval;

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

	struct jit_link *link; /* exits waiting for their target to exist */
	uint32_t  link_max;
	uint32_t  link_used;
	uint16_t *link_hash;
	uint32_t  link_hash_mask;

	int       step;        /* the instruction at s->pc is C's to execute */

	uint32_t  flushes;     /* what the run cost, for the report */
	uint32_t  blocks;
	uint32_t  bytes;
	uint32_t  declines;
	uint32_t  dec_mem;     /* ...of them, a device address or a bad width */
	uint32_t  dec_dev;
	uint32_t  dec_align;
	uint32_t  dec_jump;    /* ...a target outside RAM or off a word */
	uint32_t  fences;      /* fence.i executed, and regions it retired */
	uint32_t  stale;
	uint32_t  entries;
	uint32_t  warmups;     /* chunks the interpreter ran instead */
} rv32_jit_t;

extern rv32_jit_t rv32_jit;
/* The map itself is a static array, so that the runtime's lookup can name it
   with a link-time constant and spare itself a load from SDRAM on every
   indirect jump. */
extern uint32_t rv32_jit_map[RV32_JIT_SLOTS * 2];

/* Carve the arena into a code cache and its tables.  Returns 0 if what it was
   given is too small to be worth using, in which case the caller should fall
   back to the interpreter. */
int rv32_jit_init(void *arena, uint32_t bytes);

/* The translated code for a guest pc, translating it if need be.  NULL when
   the instruction there is one this cannot translate -- the caller then has
   the C interpreter execute exactly that one, as the assembly path does. */
uint8_t *rv32_jit_block(rv32_t *s, uint32_t pc);

/* Called by the runtime when a block gives up.  `code` names the block, the
   instruction within it and the reason.  Returns the guest pc to resume at,
   with x[] brought up to date from the spilled registers. */
uint32_t rv32_jit_fault(rv32_t *s, uint32_t code);

/* The guest has executed fence.i: it may have written code.  Nothing else
   is watched -- not stores, which RISC-V says need not be seen by an
   instruction fetch until a fence.i, and not the lr/sc reservation, which
   a translated store does not break.  Every translation is checked against
   the words it was made from, and what has changed is retired. */
void rv32_jit_fence(rv32_t *s);

/* Throw the cache away.  Safe only from outside it. */
void rv32_jit_flush(void);

/* rv32_jitrt.s */
uint32_t rv32_jit_enter(rv32_t *s, uint32_t budget, void *code);

#endif
