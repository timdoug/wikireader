/* Zstandard's standard allocation hooks, mapped to the Grifo application heap. */
#include <grifo.h>
#include <regs.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void *malloc(size_t size)
{
	return size ? memory_allocate(size, "zim-heap") : NULL;
}

void *calloc(size_t count, size_t size)
{
	void *memory;
	if (count && size > (size_t)-1 / count)
		return NULL;
	memory = malloc(count * size);
	if (memory)
		memset(memory, 0, count * size);
	return memory;
}

void free(void *memory)
{
	if (memory)
		memory_free(memory, "zim-heap");
}

/* The SDRAM controller keeps one row open per bank and pays a row change
 * whenever an access moves to another row of the same bank, so buffers read
 * or written together belong in different banks.  How far apart that is
 * cannot be taken from the controller's ADDRC field: a 32 MB board reports
 * a geometry whose banks would be 8 MB (manual II.4.1.3.2:
 * 2^(rows+columns+1) bytes), while on both units alternating reads 4, 8
 * and 16 MB apart all run at the speed of reads within one row.  The
 * boards carry a single memory device, so why the stride is 4 MB is not
 * established, but placement has to follow the hardware.  So measure it,
 * and fall back to the register.
 *
 * The measurement alternates two reads and times them: a gap inside one
 * bank pays a precharge and an activate every pair, a gap that reaches
 * another bank does not.  The smallest gap that runs at the floor is the
 * bank stride.  Reads only, anywhere in memory, so nothing is disturbed. */
#define SDRAM_START 0x10000000u

static size_t register_bank_size(void)
{
	static const unsigned char bank_shift[8] = {
		20, 21, 22, 23, 21, 22, 23, 24
	};
	return (size_t)1 << bank_shift[REG_SDRAMC_CTL & ADDRC_MASK];
}

static size_t register_total(void)
{
	static const unsigned char banks[8] = { 2, 4, 4, 4, 2, 4, 4, 4 };
	return register_bank_size() * banks[REG_SDRAMC_CTL & ADDRC_MASK];
}

static unsigned long pair_ticks(const unsigned char *a, const unsigned char *b,
				unsigned long n)
{
	unsigned long start = timer_get();
	unsigned int scratch;

	__asm__ volatile ("1:\n\tld.w\t%3, [%0]\n\tld.w\t%3, [%1]\n\t"
			  "sub\t%2, 1\n\tjrne\t1b"
			  : "+r"(a), "+r"(b), "+r"(n), "=&r"(scratch)
			  : : "memory");
	return timer_get() - start;
}

static size_t bank_size(void)
{
	static size_t measured;
	const unsigned char *base;
	unsigned long floor_ticks;
	size_t limit = register_bank_size();
	size_t gap;

	if (measured)
		return measured;
	measured = limit;   /* if nothing reads fast, keep the register's word */
	base = (const unsigned char *)(SDRAM_START + (1u << 20));
	floor_ticks = pair_ticks(base, base + 4, 2000);
	for (gap = 64u << 10; gap <= limit; gap <<= 1) {
		if ((uintptr_t)base + gap + 4 >= SDRAM_START + register_total())
			break;
		if (pair_ticks(base, base + gap, 2000) <=
		    floor_ticks + floor_ticks / 8) {
			measured = gap;
			break;
		}
	}
	return measured;
}

static int inside_one_bank(const void *memory, size_t size)
{
	uintptr_t first = (uintptr_t)memory;
	uintptr_t last = first + size - 1;
	return (first / bank_size()) == (last / bank_size());
}

/* Allocate a block that does not cross an SDRAM bank boundary, so a buffer
 * streamed sequentially keeps to one bank and leaves the others' open rows
 * alone.  The kernel allocator is first fit, so a straddling block is moved
 * up by holding the space in front of the boundary until the allocation
 * lands beyond it; the fillers are then released for smaller buffers. */
void *zim_alloc_bank_local(size_t size)
{
	void *fillers[8];
	unsigned filler_count = 0;
	void *memory = malloc(size);

	if (size >= bank_size())
		return memory;
	while (memory && !inside_one_bank(memory, size) &&
	       filler_count < sizeof(fillers) / sizeof(fillers[0])) {
		uintptr_t boundary = ((uintptr_t)memory + size) & ~(bank_size() - 1);
		/* The block's header page precedes the returned pointer; a
		 * filler reaching the boundary makes the next page start there. */
		size_t filler = (size_t)(boundary - (uintptr_t)memory);

		free(memory);
		fillers[filler_count++] = malloc(filler);
		memory = malloc(size);
	}
	while (filler_count)
		free(fillers[--filler_count]);
	return memory;
}

static unsigned bank_of(const void *memory)
{
	return (unsigned)(((uintptr_t)memory - SDRAM_START) / bank_size());
}

/* Allocate a block in a bank other than those holding `avoid` and `avoid2`
 * (either may be NULL), without
 * straddling a bank boundary.  The heap is first fit, so the block is
 * pushed forward a bank at a time by holding the space in front of the
 * next boundary; each filler is at most one bank, unlike asking for an
 * absolute bank index, which on a 32 MB board could reach for 15 MB.  A
 * bank is a preference: if the heap runs out of room the block comes from
 * wherever there is space. */
void *zim_alloc_other_banks(size_t size, const void *avoid, const void *avoid2)
{
	void *fillers[8];
	unsigned filler_count = 0;
	void *memory = malloc(size);
	size_t bytes = bank_size();

	if (size >= bytes || (!avoid && !avoid2))
		return memory;
	while (memory && filler_count < sizeof(fillers) / sizeof(fillers[0]) &&
	       ((avoid && bank_of(memory) == bank_of(avoid)) ||
		(avoid2 && bank_of(memory) == bank_of(avoid2)) ||
		!inside_one_bank(memory, size))) {
		uintptr_t boundary = ((uintptr_t)memory + bytes) & ~(uintptr_t)(bytes - 1);
		size_t filler = (size_t)(boundary - (uintptr_t)memory);

		free(memory);
		fillers[filler_count++] = malloc(filler);
		memory = malloc(size);
	}
	while (filler_count)
		free(fillers[--filler_count]);
	return memory;
}

void *zim_alloc_other_bank(size_t size, const void *avoid)
{
	return zim_alloc_other_banks(size, avoid, NULL);
}
