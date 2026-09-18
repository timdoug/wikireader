/* rv32.h - an rv32ima machine, M-mode and no MMU.
 *
 * The configuration Linux calls "nommu M-mode" (CONFIG_RISCV_M_MODE with
 * CONFIG_MMU off), which is what makes a Linux-capable core small enough to
 * hand-place in the C33's internal RAM: no Sv32 walks, no TLB, no
 * supervisor mode.  The memory map is the one mini-rv32ima established, so
 * an off-the-shelf buildroot image for that target boots unmodified.
 */

#ifndef RV32_H
#define RV32_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
	RV_RAM_BASE   = 0x80000000,  /* where guest RAM is addressed */
	RV_UART_BASE  = 0x10000000,  /* 8250, the console Linux expects */
	RV_CLINT_BASE = 0x11000000,  /* mtimecmp at +0x4000, mtime at +0xbff8 */
	RV_SYSCON     = 0x11100000,  /* poweroff/reboot, as in the dtb */
	RV_MARK       = 0x11200000,  /* not a real device: a benchmark marker */
};

/* Why rv32_run stopped. */
typedef enum {
	RV_RAN_OUT = 0,   /* executed the requested number of instructions */
	RV_POWEROFF,      /* guest wrote 0x5555 to SYSCON */
	RV_REBOOT,        /* guest wrote 0x7777 to SYSCON */
	RV_FAULT,         /* trapped with no handler installed */
} rv32_stop_t;

/* Byte offsets the assembly hot path depends on.  rv32.c asserts every one
   of them against the struct, so the two cannot drift apart silently. */
#define RV32_OFF_X          0
#define RV32_OFF_PC       128
#define RV32_OFF_RAM      132
#define RV32_OFF_RAM_SIZE 136
#define RV32_OFF_RESERVE  140

typedef struct rv32 {
	uint32_t x[32];        /* x0 is kept zero; the writeback path skips it */
	uint32_t pc;

	/* Kept together and up front because the assembly addresses them by
	   a hard-coded offset; see RV32_OFF_* above. */
	uint8_t *ram;          /* guest RAM, word-aligned, RV_RAM_BASE upward */
	uint32_t ram_size;
	uint32_t reservation;  /* lr/sc address, ~0 when none is held */

	/* Machine CSRs.  Only the ones Linux and OpenSBI-less M-mode touch. */
	uint32_t mstatus;
	uint32_t mtvec;
	uint32_t mscratch;
	uint32_t mepc;
	uint32_t mcause;
	uint32_t mtval;
	uint32_t mie;
	uint32_t mip;

	/* 64-bit counters as halves: the C33 has no 64-bit ALU and every
	   read of these is on the instruction path. */
	uint32_t cycle_lo, cycle_hi;
	uint32_t time_lo, time_hi;
	uint32_t timecmp_lo, timecmp_hi;

	bool waiting_for_interrupt;  /* in WFI; time still advances */

	/* 3 for machine, 0 for user.  There is no MMU and no supervisor, but
	   the level still has to be tracked: a syscall from userspace has to
	   raise cause 8 and one from the kernel cause 11, and Linux routes
	   only the first to its syscall path. */
	uint32_t priv;

	/* Console.  The guest's stdout arrives a byte at a time. */
	void (*putchar)(void *arg, int c);
	int (*getchar)(void *arg);   /* -1 when no byte is ready */
	void *console_arg;
	int pending_char;      /* one byte of lookahead, so LSR can be polled */

	/* Benchmark marker: the guest writes a kernel id to RV_MARK and the
	   host timestamps it, which is how per-kernel cycle costs are taken
	   without the guest needing a trustworthy clock. */
	void (*mark)(void *arg, uint32_t id);

	uint32_t stop_value;   /* SYSCON word that ended the run */
	uint64_t retired;      /* instructions executed since reset */
} rv32_t;

/* Built with RV32_FASTCODE, the interpreter is placed in the C33's A0 RAM
   instead of SDRAM.  It has to be a build switch rather than a default
   because that RAM is 5 KB in total and an application may want it for
   something else -- and because the size of the win is the measurement.
   With RV32_ASM the A0 RAM belongs to the assembly hot path instead: the C
   interpreter has become the cold fallback and stays in SDRAM. */
#if defined(RV32_FASTCODE) && !defined(RV32_ASM)
#define RV32_HOT __attribute__((section(".fastcode")))
#else
#define RV32_HOT
#endif

/* The C interpreter's dispatch table wants to be internal RAM too, but gcc
   refuses to put data in a code section and A0 RAM has no room beside the
   code.  It goes in the LCD controller's window buffer, which is internal
   RAM the framebuffer does not use, and is filled on first entry. */
#ifdef RV32_FASTCODE
#define RV32_HOTDATA __attribute__((section(".ivram_data")))
#else
#define RV32_HOTDATA
#endif

/* The device paths go in the LCD window buffer rather than in SDRAM: they
   are not the cold code they were taken for, and the assembly hot path calls
   them directly rather than declining the instruction.  Under the same
   switch as everything else, so a build that wants none of this placement
   gets none of it -- and so that the host test, which has neither section,
   compiles the same source. */
#ifdef RV32_FASTCODE
#define RV32_MMIO __attribute__((noinline)) __attribute__((section(".ivram_code")))
#else
#define RV32_MMIO __attribute__((noinline))
#endif

/* RV32_FASTSTATE puts the machine itself -- the guest register file above
   all -- in the same internal RAM.  Every guest instruction reads one to
   three registers and writes one, so in SDRAM that is several row accesses
   per instruction on top of the guest's own fetch. */
#ifdef RV32_FASTSTATE
#define RV32_STATE __attribute__((section(".fastbss")))
#else
#define RV32_STATE
#endif

/* Reset into the state a bare-metal image expects: pc at entry, a1 holding
   the device-tree address (Linux's boot protocol), a0 the hart id. */
void rv32_reset(rv32_t *s, uint32_t entry, uint32_t dtb);

/* Execute up to `budget` instructions.  Advances the guest clock by
   `time_ticks` per call, which is how fast the guest believes time runs. */
rv32_stop_t rv32_run(rv32_t *s, uint32_t budget, uint32_t time_ticks) RV32_HOT;

#endif
