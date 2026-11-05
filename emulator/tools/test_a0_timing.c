/* Identical A0 data accesses with the instruction stream on either bus.
 * The physical pipeline probe exercises these placements independently.
 */
#include <stdio.h>
#include <string.h>
#include "../src/mem.h"
#include "../src/model.h"

static int failures;
static void check(const char *name, uint64_t actual, uint64_t expected)
{
	if (actual != expected) {
		fprintf(stderr, "%s: got %llu, expected %llu\n", name,
			(unsigned long long)actual, (unsigned long long)expected);
		failures++;
	}
}

static uint64_t accesses(uint32_t pc, uint32_t address, enum mem_access access,
			 unsigned size, bool area0)
{
	struct mem mem = {0};
	mem.ivram_area0 = area0;
	wremu_cur_pc = pc;
	uint64_t elapsed = 0;
	for (unsigned i = 0; i < 20; i++)
		elapsed += mem_wait(&mem, access, address, size, elapsed);
	return elapsed;
}

int main(void)
{
	model.a0_read_half = 2;
	model.a0_read_word_half = 3;
	model.a0_write_half = 1;
	model.a0_sdram_overlap = 1;
	for (unsigned i = 0; i < 3; i++) {
		enum mem_access access = i == 2 ? MEM_CPU_WRITE : MEM_CPU_READ;
		unsigned size = i == 0 ? 2 : 4;
		unsigned half = i == 0 ? 2 : i == 1 ? 3 : 1;
		check("A0 fetch retains the calibrated shared-path cost",
		      accesses(0xc00, 0x1000, access, size, false), half * 10);
		check("SDRAM fetch overlaps half a clock of A0 data overhead",
		      accesses(SDRAM_BASE, 0x1000, access, size, false), (half - 1) * 10);
		check("mapped IVRAM data follows the local-RAM path",
		      accesses(SDRAM_BASE, IVRAM_A0_BASE, access, size, true), (half - 1) * 10);
		check("IVRAM code retains its existing timing",
		      accesses(IVRAM_BASE, 0x1000, access, size, false), half * 10);
	}
	check("the overlap does not change DMA data timing",
	      accesses(SDRAM_BASE, 0x1000, MEM_DMA_READ, 4, false), 0);
	check("unmapped IVRAM does not receive an A0 timing adjustment",
	      accesses(SDRAM_BASE, IVRAM_A0_BASE, MEM_CPU_READ, 4, false), 0);
	check("the first address beyond SDRAM is not an SDRAM fetch",
	      accesses(SDRAM_BASE + SDRAM_SIZE, 0x1000, MEM_CPU_READ, 4, false), 30);
	model.a0_sdram_overlap = 0;
	check("zero overlap restores the old SDRAM-code timing",
	      accesses(SDRAM_BASE, 0x1000, MEM_CPU_READ, 4, false), 30);
	model.a0_sdram_overlap = 100;
	check("overlap saturates without making a negative wait",
	      accesses(SDRAM_BASE, 0x1000, MEM_CPU_READ, 4, false), 0);
	model.a0_sdram_overlap = 1;
	struct mem mem = {0};
	wremu_cur_pc = SDRAM_BASE;
	check("first odd half-clock access retains its fraction",
	      mem_wait(&mem, MEM_CPU_READ, 0x1000, 2, 0), 0);
	wremu_cur_pc = 0xc00;
	check("changing fetch bus preserves a pending fraction",
	      mem_wait(&mem, MEM_CPU_READ, 0x1000, 2, 0), 1);
	wremu_cur_pc = SDRAM_BASE;
	check("a later remote fetch can finish that fractional wait",
	      mem_wait(&mem, MEM_CPU_READ, 0x1000, 2, 1), 1);
	printf("A0 data/fetch timing: %s\n", failures ? "FAIL" : "all checks passed");
	return failures != 0;
}
