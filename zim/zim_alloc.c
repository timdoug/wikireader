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
 * whenever an access moves to another row of the same bank.  Bytes per bank
 * follow the controller's ADDRC geometry (S1C33E07 manual II.4.1.3.2:
 * 2^(rows+columns+1) bytes); the boards' 16 MB setting gives 4 MB banks. */
static size_t bank_size(void)
{
	static const unsigned char bank_shift[8] = {
		20, 21, 22, 23, 21, 22, 23, 24
	};
	return (size_t)1 << bank_shift[REG_SDRAMC_CTL & ADDRC_MASK];
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
