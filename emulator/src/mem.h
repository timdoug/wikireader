/*
 * WikiReader memory map, transcribed from samo-lib/grifo/lds/grifo.lds:
 *
 *   a0ram   0x00000000   8 KiB    vectors / suspend code
 *   ivram   0x00080000  12 KiB
 *   dstram  0x00084000   2 KiB
 *   sdram   0x10000000  32 MiB    kernel at base, apps above
 *
 * Peripheral registers live at REG_BASE 0x00300000 (samo-lib/include/regs.h).
 */

#ifndef MEM_H
#define MEM_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#define A0RAM_BASE   0x00000000u
#define A0RAM_SIZE   (8u * 1024)
#define IVRAM_BASE   0x00080000u
#define IVRAM_SIZE   (12u * 1024)
#define DSTRAM_BASE  0x00084000u
#define DSTRAM_SIZE  (2u * 1024)
#define CHIP_ID_BASE 0x00020000u
#define CHIP_ID_SIZE 4u
#define SDRAM_BASE   0x10000000u
#define SDRAM_SIZE   (32u * 1024 * 1024)

#define REG_BASE     0x00300000u
#define REG_SIZE     0x00010000u

struct mem;

/* Masters and access types seen by the external-memory timing model. */
enum mem_access {
	MEM_CPU_FETCH,
	MEM_CPU_READ,
	MEM_CPU_WRITE,
	MEM_DMA_READ,
	MEM_DMA_WRITE,
};

typedef uint64_t (*mem_wait_fn)(void *ctx, enum mem_access access,
				uint32_t addr, unsigned size, uint64_t now);

/* A peripheral claims a register range; return true if the access was handled. */
typedef bool (*mmio_fn)(void *ctx, uint32_t off, unsigned size,
			uint32_t *val, bool is_write);

struct mmio_dev {
	const char *name;
	uint32_t    off, len;
	mmio_fn     fn;
	void       *ctx;
};

#define MAX_MMIO 16

struct mem {
	uint8_t *a0ram, *ivram, *dstram, *sdram;
	/*
	 * SDRAM size selected by the controller's ADDRC field, or 0 before the
	 * controller is enabled.  The chip only decodes that many address bits,
	 * so a 16 MB board aliases 0x11000000 onto 0x10000000; firmware that
	 * outgrows the configured size must fail here the way it does on
	 * hardware.  Bumped in epoch so cached region pointers are dropped.
	 */
	uint32_t sdram_alias;
	unsigned sdram_epoch;
	struct mmio_dev dev[MAX_MMIO];
	unsigned ndev;
	mem_wait_fn wait;
	void       *wait_ctx;

	/* diagnostics */
	unsigned long unmapped_reads, unmapped_writes;
	bool     trace_mmio;
	FILE    *log;

	/* write watchpoint: report any store overlapping this address */
	uint32_t        watch;
	uint32_t        vwatch; bool vwatch_on; unsigned vhits;
	bool            watch_on;
	const uint32_t *pc_src;   /* points at cpu.pc, for reporting */
};

bool mem_init(struct mem *m);
void mem_free(struct mem *m);
/* bytes must be a power of two below the full window, or 0 for no aliasing. */
void mem_set_sdram_size(struct mem *m, uint32_t bytes);
/*
 * Clear every RAM region. Cutting the power to the board loses all of it,
 * so a machine coming back on must not find the last session's contents --
 * least of all the framebuffer.
 */
void mem_clear_ram(struct mem *m);
void mem_add_mmio(struct mem *m, const char *name, uint32_t off, uint32_t len,
		  mmio_fn fn, void *ctx);
void mem_set_timing(struct mem *m, mem_wait_fn fn, void *ctx);
uint64_t mem_wait(void *ctx, enum mem_access access, uint32_t addr,
		  unsigned size, uint64_t now);

/*
 * Host pointer for a mapped address, plus the bounds of the region it falls
 * in, so callers can cache it and skip the dispatch while they stay inside.
 * Returns NULL for MMIO and unmapped addresses.
 */
uint8_t *mem_region(struct mem *m, uint32_t addr, uint32_t *base, uint32_t *len);

uint32_t mem_read(void *ctx, uint32_t addr, unsigned size);
void     mem_write(void *ctx, uint32_t addr, unsigned size, uint32_t val);

/* Load an ELF32 little-endian c33 image; returns entry point or 0 on error. */
uint32_t elf_load(struct mem *m, const char *path, char *err, size_t errlen);

#endif /* MEM_H */
