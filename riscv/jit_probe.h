/* jit_probe.h - the templates in jit_probe.s, and what they run against.
 *
 * The offsets here are the .set directives at the top of that file; nothing
 * checks them against each other but a size assertion, so keep the two in
 * step by hand as rv32.h and rv32_hot.s do.
 */

#ifndef JIT_PROBE_H
#define JIT_PROBE_H

#include <stdint.h>

struct jit_ctx {
	uint32_t *regs;        /* 0  the guest register file, in internal RAM */
	uint8_t *src;          /* 4  the copy loop's source */
	uint8_t *dst;          /* 8  ...destination */
	uint8_t *end;          /* 12 ...and where it stops */
	uint8_t *ram;          /* 16 guest RAM, as host addresses */
	uint8_t *ram_end;      /* 20 */
	uint32_t adj;          /* 24 host address = guest address + adj */
	uint32_t *table;       /* 28 the lookup the exit_hash blocks jump through */
};

/* The stride jit_probe.s lays its blocks out on, and where the first one
   starts; the driver fills in a table of their addresses after copying, the
   way a translator would, and checks the two files still agree about the
   layout before it does. */
#define JP_SLOT   56
#define JP_BLOCKS 8

typedef uint32_t (*jp_fn)(uint32_t passes, struct jit_ctx *ctx);

/* name, guest instructions a pass stands for, passes, repeats, check.
 *
 * The check says what the driver can look at afterwards to know the template
 * ran to the end: JP_WALK that the base register stepped once a pass,
 * JP_STREAM that it reached the end of the stream, JP_COPIED that the copy
 * arrived, JP_NONE for the two that touch no memory at all.
 *
 * A pass of the load templates reads sixteen bytes and steps the base
 * register over them, so the passes are what keeps the walk inside the
 * stream; the repeats are what make the run long enough to time.  Zero passes
 * means the template runs the stream to its end and counts them itself.
 *
 * The repeats also keep any one call under a tenth of a second, because the
 * watchdog is only kicked between them: on the device a run that overstays it
 * does not report a slow number, it restarts the machine. */
enum { JP_NONE, JP_WALK, JP_STREAM, JP_COPIED, JP_DRAINED };

#define JP_TEMPLATES(X) \
	X(alu_reg,   60,  10000,  4, JP_NONE) \
	X(alu_mem,    8,  50000,  5, JP_NONE) \
	X(ld_free,    5,  16000, 24, JP_WALK) \
	X(ld_check,   5,  16000, 24, JP_WALK) \
	X(copy_reg,   5,      0,  6, JP_COPIED) \
	X(copy_mem,   5,      0,  6, JP_STREAM) \
	X(exit_none, 64,  10000,  3, JP_DRAINED) \
	X(exit_link, 64,  10000,  3, JP_DRAINED) \
	X(exit_hash, 64,  10000,  3, JP_DRAINED)

#define JP_DECLARE(name, guest, passes, repeats, check) \
	uint32_t jp_##name(uint32_t, struct jit_ctx *); \
	extern const uint8_t jp_##name##_end[];
JP_TEMPLATES(JP_DECLARE)
#undef JP_DECLARE

extern const uint8_t jp_exit_hash_blocks[];

#endif
