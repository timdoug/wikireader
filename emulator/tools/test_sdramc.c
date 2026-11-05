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

static void write_width(struct mem *mem, struct sdramc *s)
{
	struct model saved = model;
	model.write_post = model.row_ports = model.act_overlap = 0;
	model.row_change_extra = 0;
	static const unsigned calibration[] = {0, 1, 8, 12};
	static const unsigned fixed_overhead[] = {0, 0, 4, 8};
	for (unsigned half = 2; half <= 4; half += 2)
	for (unsigned dbf = 0; dbf < 2; dbf++)
	for (unsigned k = 0; k < sizeof calibration / sizeof *calibration; k++)
	for (unsigned dma = 0; dma < 2; dma++) {
		model.sdclk_half = half;
		model.wr_ticks = calibration[k];
		unsigned tick = dbf ? half / 2 : half;
		/* Prime one row, and compare uncrowded writes after it settles.
		 * The external duration is independent of the bus master. */
		timing_setup(mem, s, 0x8000000b | (dbf ? 1u << 5 : 0), 0xfff);
		mem_wait(mem, MEM_CPU_WRITE, SDRAM_BASE, 4, 0);
		for (unsigned size = 1; size <= 4; size *= 2) {
			unsigned now = 100 * size;
			uint64_t duration = fixed_overhead[k] + (size < 4 ? 1 : 2) * tick;
			uint64_t wait = mem_wait(mem, dma ? MEM_DMA_WRITE : MEM_CPU_WRITE,
				SDRAM_BASE + 8, size, now);
			check64("write completion follows external 16-bit beat count",
				s->bus_free - 2 * now, duration);
			check64("CPU/DMA waits include exactly the external write duration",
				wait, (duration + 1) / 2);
		}
	}
	model = saved;
}

static void accepted_alias_writes(struct mem *mem, struct sdramc *s)
{
	struct model saved = model;
	model.iq_lookahead = model.dq_hit = model.row_ports = 0;
	model.act_overlap = model.row_change_extra = 0;
	const uint32_t low = SDRAM_BASE + 0x1000;
	const uint32_t high = low + 16u * 1024 * 1024;
	for (unsigned policy = 0; policy <= 2; policy++)
	for (unsigned size = 1; size <= 4; size *= 2) {
		model.iq_deferred = policy;
		timing_setup(mem, s, 0x8000000b, 0xfff);
		mem_wait(mem, MEM_CPU_FETCH, low, 2, 0);
		mem_wait(mem, MEM_CPU_FETCH, high, 2, 100);
		mem_wait(mem, MEM_CPU_FETCH, high + 14, 2, 200);
		mem_wait(mem, MEM_CPU_READ, low, 4, 300);
		mem_write(mem, high + 16, size, 0);
		check64("unrelated accepted store preserves the first cached alias",
			mem_wait(mem, MEM_CPU_FETCH, low, 2, 1000), 0);
		check64("unrelated accepted store preserves the second cached alias",
			mem_wait(mem, MEM_CPU_FETCH, high, 2, 1000), 0);
		mem_write(mem, high, size, 0x12345678);
		unsigned iq = 0, dq = 0;
		for (unsigned i = 0; i < 2; i++)
			iq += s->iq[i].valid;
		for (unsigned i = 0; i < SDRAMC_MAX_DQ; i++)
			dq += s->dq[i].valid;
		check64("accepted store invalidates both physical instruction aliases", iq, 0);
		check64("accepted store invalidates data cached through another alias", dq, 0);
		uint32_t mask = size == 4 ? 0xffffffffu : (1u << (8 * size)) - 1;
		check64("cache invalidation accompanies the actual aliased RAM change",
			mem_read(mem, low, size), 0x12345678 & mask);
	}
	model = saved;
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
	mem_write(&mem, SDRAM_BASE + 0x100, 4, 0x12345678);
	check64("write flushes matching DQB data",
		mem_wait(&mem, MEM_CPU_READ, SDRAM_BASE + 0x100, 4, 30), 3);
	check64("changing row observes precharge and activation timings",
		mem_wait(&mem, MEM_CPU_READ, SDRAM_BASE + 0x400, 4, 34), 11);
	check64("IQB hit counter records buffered instruction fetch",
		sdramc.iq_hits, 1);
	check64("DQB hits include DMA reads",
		sdramc.dq_hits, 1);

	/* A posted write stops the CPU only for handing it over, write_issue,
	 * and the bus stays busy behind it: the device copies a word at a
	 * time faster than four at a time, and a store that blocked until the
	 * bus had taken it could not do that.
	 */
	model.write_post = 1;
	timing_setup(&mem, &sdramc, 0x8000000b, 0x00000fff);
	check64("a posted write holds the CPU only to hand it over",
		mem_wait(&mem, MEM_CPU_WRITE, SDRAM_BASE, 4, 0),
		(model.write_issue + 1) / 2);
	check64("...but the next write waits for the buffer to drain",
		mem_wait(&mem, MEM_CPU_WRITE, SDRAM_BASE + 4, 4, 1), 5);
	timing_setup(&mem, &sdramc, 0x8000000b, 0x00000fff);
	mem.dma_bus_free = 100;
	check64("posted CPU handoff waits for DMA bus ownership to end",
		mem_wait(&mem, MEM_CPU_WRITE, SDRAM_BASE, 4, 0),
		100 + (model.write_issue + 1) / 2);
	mem.dma_bus_free = 0;
	/* Changing the CPU's internal handoff cost must not retime a DMA
	 * stream when its acceptance cost is held fixed. Check a sequence
	 * with buffer pressure, not only an empty-buffer scalar wait. */
	uint64_t dma_elapsed[2], dma_bus_end[2];
	unsigned saved_issue = model.write_issue;
	model.dma_write_issue = saved_issue;
	for (unsigned trial = 0; trial < 2; trial++) {
		model.write_issue = trial ? 20 : saved_issue;
		timing_setup(&mem, &sdramc, 0x8000000b, 0x00000fff);
		uint64_t clock = 0;
		for (unsigned i = 0; i < 16; i++)
			clock += 1 + mem_wait(&mem, MEM_DMA_WRITE, SDRAM_BASE + 4 * i, 4, clock);
		dma_elapsed[trial] = clock;
		dma_bus_end[trial] = sdramc.bus_free;
	}
	check64("fixed DMA handoff is independent of CPU store overhead",
		dma_elapsed[1], dma_elapsed[0]);
	check64("separate acceptance costs preserve the external write schedule",
		dma_bus_end[1], dma_bus_end[0]);
	model.write_issue = saved_issue;
	model.dma_write_issue = 0;
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
	/* ...except that since model.bank_floors it does not: tRAS, tRP and
	   tRC are charged against the physical bank, and four megabytes away
	   is a bank with nothing open in it, so the model charges tRCD alone
	   and comes out tRP cheaper than the kilobyte. The device says the
	   two are within 1% of each other, so this number is the model's and
	   not the machine's -- it is the distant-stream error, recorded here
	   rather than asserted away. */
	check64("...and one four megabytes away skips the precharge",
		mem_wait(&mem, MEM_CPU_READ, SDRAM_BASE + 0x400000, 4, 20), 7);
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
	/* The refresh waits tRP + tRFC and leaves every bank precharged, so
	   the read after it activates and waits tRCD without paying the
	   precharge or the array recovery a second time. */
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
	/* Both of these close every bank, so the read that follows activates
	   one and waits tRCD for it, two more than these asked for while the
	   tRCD charge was missing. */
	check64("self-refresh exit adds tXSR+1 before a cold read",
		mem_wait(&mem, MEM_CPU_READ, SDRAM_BASE + 4, 4, 140), 23);
	check64("self-refresh exit is counted", sdramc.self_refresh_exits, 1);

	sdramc_reset(&sdramc);
	check("power cycle restores the refresh counters",
	      mem_read(&mem, REF, 4), 0x007f008c);

	/* Refresh precharges every physical bank, including banks 4--7 on
	 * the measured 32 MiB geometry. A write opens bank 7 at MCLK 19 and
	 * finishes at 25; tRAS=8 forbids precharge until 27. With tWR=5 it
	 * must instead wait until 30. Refresh itself then costs tRP+tRFC=19.
	 * Exercise both physical-bank and read/write-port row bookkeeping. */
	for (unsigned ports = 0; ports <= 1; ports++) {
		model.row_ports = ports;
		model.bank_floors = 1;
		for (unsigned recovery = 0; recovery <= 5; recovery += 5) {
			model.write_recovery = recovery;
			timing_setup(&mem, &sdramc, 0x8000000b, 19);
			mem_write(&mem, CTL, 4, 0x37e3); /* 32 MiB, 8 banks */
			mem_wait(&mem, MEM_CPU_READ, SDRAM_BASE, 4, 0);
			check64("late bank-7 DMA write finishes at MCLK 25",
				mem_wait(&mem, MEM_DMA_WRITE,
					 SDRAM_BASE + 7 * 0x400000, 4, 19), 6);
			mem_wait(&mem, MEM_CPU_READ, SDRAM_BASE + 4, 4, 25);
			check64(recovery ? "refresh honors bank-7 write recovery" :
					   "refresh honors bank-7 minimum active time",
				sdramc.array_free, 2 * (recovery ? 49 : 46));
			check64("late-bank refresh is counted once", sdramc.refreshes, 1);
		}
	}
	model.write_recovery = 0;
	model.row_ports = 0;
	/* Refresh is later than the write's 9-cycle turnaround already.
	 * Adding that delay to the refresh end would charge it twice. */
	model.wr_rd_turn = 9;
	timing_setup(&mem, &sdramc, 0x8000000b, 19);
	mem_write(&mem, CTL, 4, 0x37e3);
	mem_wait(&mem, MEM_CPU_READ, SDRAM_BASE, 4, 0);
	mem_wait(&mem, MEM_DMA_WRITE, SDRAM_BASE + 7 * 0x400000, 4, 19);
	check64("refresh absorbs an already elapsed read turnaround",
		mem_wait(&mem, MEM_CPU_READ, SDRAM_BASE + 4, 4, 25), 28);

	/* A previous controller lifetime, or a different controller instance,
	 * cannot impose a write-to-read turnaround on this one's clean read. */
	model.wr_rd_turn = 9;
	timing_setup(&mem, &sdramc, 0x8000000b, 0xfff);
	mem_wait(&mem, MEM_CPU_WRITE, SDRAM_BASE, 4, 100);
	timing_setup(&mem, &sdramc, 0x8000000b, 0xfff);
	check64("reset discards previous write-to-read history",
		mem_wait(&mem, MEM_CPU_READ, SDRAM_BASE, 4, 0), 7);

	struct mem other;
	struct sdramc other_sdramc;
	if (!mem_init(&other))
		return 1;
	sdramc_attach(&other, &other_sdramc);
	timing_setup(&other, &other_sdramc, 0x8000000b, 0xfff);
	mem_wait(&other, MEM_CPU_READ, SDRAM_BASE, 4, 0);
	mem_wait(&mem, MEM_CPU_WRITE, SDRAM_BASE + 8, 4, 100);
	check64("other controller's write cannot delay this read",
		mem_wait(&other, MEM_CPU_READ, SDRAM_BASE + 4, 4, 8), 3);
	check64("other controller's read cannot erase write turnaround",
		mem_wait(&mem, MEM_CPU_READ, SDRAM_BASE + 12, 4, 102), 12);
	mem_free(&other);

	write_width(&mem, &sdramc);
	accepted_alias_writes(&mem, &sdramc);
	mem_free(&mem);
	printf("\n%s\n", fails ? "FAILURES" : "all SDRAMC tests passed");
	return fails != 0;
}
