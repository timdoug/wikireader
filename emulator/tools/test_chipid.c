/* S1C33E07 fixed chip-identification area, Technical Manual I.5.2. */
#include <stdio.h>

#include "../src/mem.h"

static int fails;

static void check(const char *what, uint32_t got, uint32_t want)
{
	printf("%-62s %s\n", what, got == want ? "ok" : "FAIL");
	if (got != want)
		fails++;
}

int main(void)
{
	struct mem mem;

	if (!mem_init(&mem))
		return 1;
	check("core ID identifies the C33 PE little-endian core",
	      mem_read(&mem, CHIP_ID_BASE + 0, 1), 0x06);
	check("product ID identifies the S1C33E series",
	      mem_read(&mem, CHIP_ID_BASE + 1, 1), 0x0e);
	check("model ID identifies the S1C33E07",
	      mem_read(&mem, CHIP_ID_BASE + 2, 1), 0x07);
	check("version ID has the documented value",
	      mem_read(&mem, CHIP_ID_BASE + 3, 1), 0x21);
	check("the little-endian word view contains all four bytes",
	      mem_read(&mem, CHIP_ID_BASE, 4), 0x21070e06);
	mem_write(&mem, CHIP_ID_BASE, 4, 0xffffffff);
	check("the identification area is read-only",
	      mem_read(&mem, CHIP_ID_BASE, 4), 0x21070e06);
	check("identification reads are mapped, not diagnostic holes",
	      mem.unmapped_reads, 0);

	mem_free(&mem);
	printf("\n%s\n", fails ? "FAILURES" : "all chip-ID tests passed");
	return fails != 0;
}
