/* SDRAM controller reset and status semantics from the S1C33E07 manual. */
#include <stdio.h>

#include "../src/mem.h"
#include "../src/sdramc.h"

#define INI (REG_BASE + 0x1600)
#define CTL (REG_BASE + 0x1604)
#define REF (REG_BASE + 0x1608)
#define APP (REG_BASE + 0x1610)

#define SDON   (1u << 4)
#define SDEN   (1u << 3)
#define INIMRS (1u << 2)
#define SELDO  (1u << 25)
#define SELEN  (1u << 23)

static int fails;

static void check(const char *what, uint32_t got, uint32_t want)
{
	printf("%-62s %s\n", what, got == want ? "ok" : "FAIL");
	if (got != want) {
		printf("    got 0x%x, wanted 0x%x\n", got, want);
		fails++;
	}
}

int main(void)
{
	struct mem mem;
	struct sdramc sdramc;

	if (!mem_init(&mem))
		return 1;
	sdramc_attach(&mem, &sdramc);

	check("initial register resets disabled", mem_read(&mem, INI, 4), 0);
	check("timing register has its documented reset value",
	      mem_read(&mem, CTL, 4), 0x000000e0);
	check("refresh counters have their documented reset values",
	      mem_read(&mem, REF, 4), 0x007f008c);
	check("application register resets to CAS latency 2",
	      mem_read(&mem, APP, 4), 0x00000008);

	mem_write(&mem, CTL, 4, 0xffffffff);
	check("reserved timing bits read zero", mem_read(&mem, CTL, 4),
	      0x000037f7);
	mem_write(&mem, APP, 4, 0xffffffff);
	check("reserved application bits read zero", mem_read(&mem, APP, 4),
	      0x8000003f);

	mem_write(&mem, INI, 4, SDON | INIMRS | SDEN | 0xffff0000);
	check("MRS completion raises read-only SDEN", mem_read(&mem, INI, 4),
	      SDON | INIMRS | SDEN);
	mem_write(&mem, INI, 4, SDON);
	check("SDEN remains set while the controller stays enabled",
	      mem_read(&mem, INI, 4), SDON | SDEN);
	mem_write(&mem, INI, 4, 0);
	check("disabling SDON clears SDEN", mem_read(&mem, INI, 4), 0);

	mem_write(&mem, REF, 4, SELEN | SELDO | 0xfc00f000);
	check("SELDO is status, not writable", mem_read(&mem, REF, 4),
	      SELEN | SELDO);
	mem_write(&mem, REF, 4, 0);
	check("leaving self-refresh clears SELDO", mem_read(&mem, REF, 4), 0);

	sdramc_reset(&sdramc);
	check("power cycle restores the refresh counters",
	      mem_read(&mem, REF, 4), 0x007f008c);

	mem_free(&mem);
	printf("\n%s\n", fails ? "FAILURES" : "all SDRAMC tests passed");
	return fails != 0;
}
