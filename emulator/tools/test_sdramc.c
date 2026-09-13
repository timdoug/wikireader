/* SDRAM controller reset and status semantics from the S1C33E07 manual. */
#include <stdio.h>

#include "../src/mem.h"
#include "../src/sdramc.h"
#include "../src/model.h"

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

static void check64(const char *what, uint64_t got, uint64_t want)
{
	printf("%-62s %s\n", what, got == want ? "ok" : "FAIL");
	if (got != want) {
		printf("    got %llu, wanted %llu\n",
		       (unsigned long long)got, (unsigned long long)want);
		fails++;
	}
}

static void timing_setup(struct mem *mem, struct sdramc *s, uint32_t app,
			 uint32_t refresh)
{
	sdramc_reset(s);
	mem_write(mem, CTL, 4, 0x37e2); /* tRP=4, tRAS=8, tRC=15, 16 MiB */
	mem_write(mem, REF, 4, refresh);
	mem_write(mem, APP, 4, app);
	mem_write(mem, INI, 4, SDON | INIMRS);
	mem_write(mem, INI, 4, SDON);
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

	/*
	 * Manual II.4.1.3 and II.4.2: CAS2, tRCD4, an eight-halfword
	 * instruction slot, and a two-halfword data buffer. Times passed to
	 * mem_wait are the CPU's MCLK count after preceding operations.
	 *
	 * The first halfword lands on the CAS cycle itself and not the one
	 * after it, which is what CAS latency means and what the device
	 * measures: every figure below is one less than it was, because the
	 * model used to charge that extra cycle on every read.
	 */
	/* These are the manual's figures; the fitted controller overheads in
	   model.c come on top of them, so hold them at zero here. The clock
	   goes back to one SDCLK per MCLK for the same reason: the checks
	   below count the manual's SDCLK, and the device's own divider --
	   two MCLK to the SDCLK, which is what model.c now defaults to --
	   would double every one of them. */
	model.iqb_first = model.iqb_word_gap = model.dq_extra = model.wr_ticks = 0;
	model.wr_rd_turn = model.dq_iram_extra = model.dq_hit = 0;
	model.row_change_extra = 0;
	model.write_post = 0;  /* what the bus does, not what the CPU waits */
	model.sdclk_half = 2;
	model.row_ports = 0;   /* the manual's per-bank rows; see below */
	timing_setup(&mem, &sdramc, 0x8000000b, 0x00000fff);
	check64("cold IQB fetch waits tRCD + CAS + first data",
		mem_wait(&mem, MEM_CPU_FETCH, SDRAM_BASE, 2, 0), 6);
	check64("next prefetched IQB halfword has no wait",
		mem_wait(&mem, MEM_CPU_FETCH, SDRAM_BASE + 2, 2, 8), 0);
	check64("next IQB line waits only CAS after previous burst",
		mem_wait(&mem, MEM_CPU_FETCH, SDRAM_BASE + 16, 2, 14), 2);
	check64("cold 32-bit DQB read waits for both burst halfwords",
		mem_wait(&mem, MEM_CPU_READ, SDRAM_BASE + 0x100, 4, 24), 3);
	check64("DQB hit inserts no SDRAM wait",
		mem_wait(&mem, MEM_DMA_READ, SDRAM_BASE + 0x102, 2, 28), 0);
	check64("32-bit write takes two individual bus operations",
		mem_wait(&mem, MEM_CPU_WRITE, SDRAM_BASE + 0x100, 4, 28), 2);
	check64("write flushes matching DQB data",
		mem_wait(&mem, MEM_CPU_READ, SDRAM_BASE + 0x100, 4, 30), 3);
	check64("changing row observes precharge and activation timings",
		mem_wait(&mem, MEM_CPU_READ, SDRAM_BASE + 0x400, 4, 34), 11);
	check64("IQB hit counter records buffered instruction fetch",
		sdramc.iq_hits, 1);
	check64("DQB hits include DMA reads",
		sdramc.dq_hits, 1);

	/* A posted write does not stop the CPU, but the bus is still busy:
	 * the device copies a word at a time faster than four at a time, and
	 * a store that blocked until the bus had taken it could not do that.
	 */
	model.write_post = 1;
	timing_setup(&mem, &sdramc, 0x8000000b, 0x00000fff);
	check64("a posted write does not hold the CPU up",
		mem_wait(&mem, MEM_CPU_WRITE, SDRAM_BASE, 4, 0), 0);
	check64("...but the next write waits for the buffer to drain",
		mem_wait(&mem, MEM_CPU_WRITE, SDRAM_BASE + 4, 4, 1), 5);
	model.write_post = 0;

	/* What the device does instead of the manual's per-bank rows: two data
	 * addresses evict one another however far apart they are. Reading two
	 * a kilobyte apart alternately costs it 141.75 cycles a pass and two
	 * four megabytes apart -- another bank, by the geometry table -- costs
	 * 140.10, so the second must cost the model what the first does.
	 */
	model.row_ports = 1;
	timing_setup(&mem, &sdramc, 0x8000000b, 0x00000fff);
	mem_wait(&mem, MEM_CPU_READ, SDRAM_BASE, 4, 0);
	check64("a read a kilobyte away changes rows",
		mem_wait(&mem, MEM_CPU_READ, SDRAM_BASE + 0x400, 4, 20), 11);
	timing_setup(&mem, &sdramc, 0x8000000b, 0x00000fff);
	mem_wait(&mem, MEM_CPU_READ, SDRAM_BASE, 4, 0);
	check64("...and one four megabytes away costs the same",
		mem_wait(&mem, MEM_CPU_READ, SDRAM_BASE + 0x400000, 4, 20), 11);
	model.row_ports = 0;

	/* DBF makes one SDCLK half of what it otherwise is; waits round up to
	   whole MCLKs. Three, not four, since the first halfword arrives on
	   the CAS cycle. */
	timing_setup(&mem, &sdramc, 0x8000002b, 0x00000fff);
	check64("double-frequency cold fetch is three MCLKs",
		mem_wait(&mem, MEM_CPU_FETCH, SDRAM_BASE, 2, 0), 3);

	/* AURCO begins at zero: 0x8c therefore expires every 141 SDCLKs. */
	timing_setup(&mem, &sdramc, 0x8000000b, 0x0000008c);
	check64("prime DQB before refresh",
		mem_wait(&mem, MEM_CPU_READ, SDRAM_BASE, 4, 0), 7);
	check64("due auto-refresh adds tRP + tRFC before a cold read",
		mem_wait(&mem, MEM_CPU_READ, SDRAM_BASE + 4, 4, 145), 22);
	check64("auto-refresh counter records the issued refresh",
		sdramc.refreshes, 1);

	/* SELCO=127 expires before AURCO=140, as configured by grifo. */
	timing_setup(&mem, &sdramc, 0x8000000b,
		     SELEN | (0x7f << 16) | 0x8c);
	check64("prime DQB before self-refresh",
		mem_wait(&mem, MEM_CPU_READ, SDRAM_BASE, 4, 0), 7);
	check64("buffer hit does not wake SDRAM from self-refresh",
		mem_wait(&mem, MEM_CPU_READ, SDRAM_BASE, 4, 140), 0);
	check64("buffer hit records no self-refresh exit",
		sdramc.self_refresh_exits, 0);
	check64("self-refresh exit adds tXSR+1 before a cold read",
		mem_wait(&mem, MEM_CPU_READ, SDRAM_BASE + 4, 4, 140), 23);
	check64("self-refresh exit is counted", sdramc.self_refresh_exits, 1);

	sdramc_reset(&sdramc);
	check("power cycle restores the refresh counters",
	      mem_read(&mem, REF, 4), 0x007f008c);

	mem_free(&mem);
	printf("\n%s\n", fails ? "FAILURES" : "all SDRAMC tests passed");
	return fails != 0;
}
