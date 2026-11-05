/* Isolated C33 instructions and the real SPI/DMA devices share one clock.
 * A receive completes while instruction fetch waits. Its write must precede
 * the instruction's later data access, without waiting for c33_step to end.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/c33.h"
#include "../src/cmu.h"
#include "../src/dma.h"
#include "../src/itc.h"
#include "../src/model.h"
#include "../src/port.h"
#include "../src/sdcard.h"
#include "../src/sdramc.h"

#define REG(a) (REG_BASE + (a))
#define ENTRY 0x100u
#define DEST (SDRAM_BASE + 0x1000u)

struct fixture {
	struct mem mem;
	struct c33 cpu;
	struct cmu cmu;
	struct itc itc;
	struct port port;
	struct sdcard sd;
	struct dma dma;
	bool remap;
	uint64_t last_data_clock;
};

static unsigned failures;

static void check(const char *label, uint32_t got, uint32_t expected)
{
	printf("%-67s %s\n", label, got == expected ? "ok" : "FAIL");
	if (got != expected) {
		printf("  got %08x, expected %08x\n", got, expected);
		failures++;
	}
}

static uint64_t fetch_wait(void *ctx, uint32_t addr, uint64_t now)
{
	return 100;
}

static uint64_t no_fetch_wait(void *ctx, uint32_t addr, uint64_t now)
{
	return 0;
}

static uint8_t *code_only_region(void *ctx, uint32_t addr, uint32_t *base, uint32_t *len)
{
	/* Keep fetch timing fixed while testing an uncached data path. */
	return addr < A0RAM_SIZE ? mem_region(ctx, addr, base, len) : NULL;
}

static uint64_t generic_wait(void *ctx, enum mem_access access,
			     uint32_t addr, unsigned size, uint64_t now)
{
	return access == MEM_CPU_FETCH ? 100 : mem_wait(ctx, access, addr, size, now);
}

static void settle(void *ctx)
{
	struct fixture *f = ctx;
	f->last_data_clock = f->cpu.clk;
	sd_poll(&f->sd);
	dma_poll(&f->dma);
	if (f->remap)
		mem_set_sdram_size(&f->mem, 16u * 1024 * 1024);
}

static void setup(struct fixture *f, uint16_t insn, bool fast, bool generic)
{
	memset(f, 0, sizeof *f);
	assert(mem_init(&f->mem));
	itc_attach(&f->mem, &f->itc);
	cmu_attach(&f->mem, &f->cmu);
	port_attach(&f->mem, &f->port, &f->itc);
	assert(sd_attach(&f->mem, &f->sd, NULL, &f->port, NULL, true));
	dma_attach(&f->mem, &f->dma, &f->itc, &f->cmu, &f->sd);
	f->cpu.bus = (struct c33_bus){
		.read = mem_read, .write = mem_write, .ctx = &f->mem,
		.region = fast ? (uint8_t *(*)(void *, uint32_t, uint32_t *, uint32_t *))mem_region : NULL,
		.wait = generic ? generic_wait : mem_wait,
		.fetch_wait = generic ? NULL : fetch_wait,
		.before_data = settle, .before_data_ctx = f,
	};
	f->cpu.region_epoch = &f->mem.region_epoch;
	c33_reset(&f->cpu, ENTRY);
	assert(f->cpu.bus.before_data == settle); /* reset preserves wiring */
	sd_set_clock(&f->sd, &f->cpu.clk);
	dma_set_clock(&f->dma, &f->cpu.clk);
	dma_set_cpu_sleeping(&f->dma, &f->cpu.sleeping);
	mem_write(&f->mem, ENTRY, 2, insn);
	f->cpu.r[1] = DEST;
	f->port.reg[OFF_P3D] |= 1u << 3; /* level buffer on */
	f->port.reg[OFF_P5D] |= 1u << CS_SDCARD_BIT; /* card deselected */
	/* One 8-bit receive, dual-address HSDMA3: SPI RXD -> RAM. */
	mem_write(&f->mem, REG(0x1708), 4, (7u << 10) | 15u);
	mem_write(&f->mem, REG(0x1710), 4, 0);
	mem_write(&f->mem, REG(0x119c), 2, 1);
	mem_write(&f->mem, REG(0x1150), 2, 1);
	mem_write(&f->mem, REG(0x1152), 2, 0x8000);
	mem_write(&f->mem, REG(0x115a), 2, 0x3000);
	mem_write(&f->mem, REG(0x1194), 4, REG(0x1700));
	mem_write(&f->mem, REG(0x1198), 4, DEST);
	mem_write(&f->mem, REG(0x0299), 1, 0x90);
	mem_write(&f->mem, REG(0x115c), 2, 1);
	mem_write(&f->mem, REG(0x1704), 4, 0xff);
	assert(f->sd.deadline == 32);
}

static void address_phase(void)
{
	static const struct {
		uint16_t insn;
		unsigned size;
		bool store, sign_byte;
		unsigned bit_op; /* 1=set, 2=clear, 3=toggle, 4=test */
	} cases[] = {
		{0x3010, 4, false, false, 0}, {0x2810, 2, false, false, 0}, {0x2c10, 2, false, false, 0},
		{0x2010, 1, false, true, 0}, {0x2410, 1, false, false, 0},
		{0x3c10, 4, true, false, 0}, {0x3810, 2, true, false, 0}, {0x3410, 1, true, false, 0},
		{0xb010, 1, false, false, 1}, {0xac10, 1, false, false, 2},
		{0xb410, 1, false, false, 3}, {0xa810, 1, false, false, 4},
	};
	for (unsigned fast = 0; fast < 2; fast++)
	for (unsigned generic = 0; generic < 2; generic++)
	for (unsigned prefixes = 0; prefixes <= 2; prefixes++)
	for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
		struct fixture f;
		setup(&f, 0xc000, fast, false); /* ext 0 */
		f.cpu.bus.wait = mem_wait;
		f.cpu.bus.fetch_wait = generic ? NULL : no_fetch_wait;
		if (!fast)
			f.cpu.bus.region = code_only_region;
		for (unsigned p = 0; p < prefixes; p++)
			mem_write(&f.mem, ENTRY + 2 * p, 2, 0xc000);
		mem_write(&f.mem, ENTRY + 2 * prefixes, 2, cases[i].insn);
		f.cpu.r[0] = cases[i].store ? 0x1234abcd : 0;
		/* SPI completes at 32; its real read/write DMA phases land at
		 * 34. Both instruction sequences reach the base load/store at
		 * 33. Its extended-address phase must precede the bus access. */
		f.cpu.clk = 33 - prefixes;
		for (unsigned p = 0; p < prefixes; p++) {
			sd_poll(&f.sd); dma_poll(&f.dma);
			c33_step(&f.cpu);
		}
		sd_poll(&f.sd); dma_poll(&f.dma);
		assert(f.cpu.clk == 33 && f.dma.qn == 1 && f.dma.q[0].due == 34);
		c33_step(&f.cpu);
		check("address phase is before the bus and only charged with an extension",
		      f.last_data_clock, 33 + (prefixes != 0));
		check("moving the address phase preserves total instruction clocks", f.cpu.clk,
		      33 + (cases[i].bit_op ? cases[i].bit_op == 4 ? 2 : 3 : 1) + (prefixes != 0));
		sd_poll(&f.sd); dma_poll(&f.dma);
		if (cases[i].bit_op) {
			if (cases[i].bit_op == 4)
				check("bit test observes DMA only after its actual completion",
				      f.cpu.sr[SR_PSR] & PSR_Z, prefixes ? 0 : PSR_Z);
			else
				check("bit update follows DMA only after its actual completion",
				      mem_read(&f.mem, DEST, 1), !prefixes || cases[i].bit_op == 1 ? 0xff : 0xfe);
		} else if (cases[i].store) {
			uint32_t mask = cases[i].size == 4 ? 0xffffffffu :
				       (1u << (8 * cases[i].size)) - 1;
			uint32_t expected = 0x1234abcdu & mask;
			if (!prefixes)
				expected = (expected & ~0xffu) | 0xff;
			check("store ordering follows the actual SPI DMA destination deadline",
			      mem_read(&f.mem, DEST, 4), expected);
		} else {
			check("load sees DMA completed during extended address calculation",
			      f.cpu.r[0], prefixes ? cases[i].sign_byte ? 0xffffffffu : 0xff : 0);
		}
		mem_free(&f.mem);
	}
}

static uint64_t frontend_wait(void *ctx, enum mem_access access,
                             uint32_t addr, unsigned size, uint64_t now)
{
	return addr == DEST && (access == MEM_CPU_READ || access == MEM_CPU_WRITE) ? 200 : 0;
}

static void frontend_dma_order(void)
{
	struct model saved = model;
	model.local_frontend = 1;
	model.dma_extra = 100;
	for (unsigned store = 0; store < 2; store++) {
		struct fixture f;
		setup(&f, store ? 0x3c10 : 0x3010, true, false);
		f.cpu.bus.fetch_wait = no_fetch_wait;
		f.cpu.bus.wait = frontend_wait;
		f.cpu.r[0] = store ? 0x1234abcd : 0;
		/* Receive is pending before E starts, but its destination completes
		 * during the CPU data wait. Use real devices with local prefetch active. */
		f.cpu.clk = 33;
		sd_poll(&f.sd); dma_poll(&f.dma);
		assert(f.dma.qn == 1 && f.dma.q[0].due == 134);
		c33_step(&f.cpu);
		check("local frontend remains active during the DMA overlap", f.cpu.frontend_valid, true);
		check("CPU data wait settles the earlier DMA completion", f.dma.qn, 0);
		check(store ? "frontend CPU store follows the DMA write" :
		      "frontend CPU load observes the DMA write",
		      store ? mem_read(&f.mem, DEST, 4) : f.cpu.r[0], store ? 0x1234abcd : 0xff);
		mem_free(&f.mem);
	}
	model = saved;
}

static void instruction_fetch_order(void)
{
	struct model saved = model;
	for (unsigned fast = 0; fast < 2; fast++)
	for (unsigned generic = 0; generic < 2; generic++)
	for (unsigned remap = 0; remap < 2; remap++) {
		struct fixture f;
		setup(&f, 0, fast, generic);
		/* SPI's real byte write changes ext 0x1f00 into ext 0x1fff
		 * while fetch is waiting. Prefix execution itself has no data
		 * access to accidentally settle the event for us. */
		mem_write(&f.mem, DEST, 2, 0xdf00);
		f.cpu.pc = DEST + (remap ? 0x1000000 : 0);
		if (remap) {
			f.remap = true;
			mem_write(&f.mem, f.cpu.pc, 2, 0xc000);
			if (fast) {
				uint32_t base, len;
				f.cpu.fetch_ptr = mem_region(&f.mem, f.cpu.pc, &base, &len);
				f.cpu.fetch_lo = base;
				f.cpu.fetch_hi = base + len;
				f.cpu.region_epoch_seen = f.mem.region_epoch;
			}
		}
		sd_poll(&f.sd); dma_poll(&f.dma);
		c33_step(&f.cpu);
		check("instruction fetch observes DMA completed during its wait", f.cpu.ext[0], 0x1fff);
		check("instruction bytes contain the completed SPI DMA write", mem_read(&f.mem, DEST, 2), 0xdfff);
		check("fetch settles devices at its response boundary", f.last_data_clock, 100);
		check("fetch response still retires exactly one prefix", f.cpu.n_ext, 1);
		check("event settlement adds no artificial fetch cycles", f.cpu.clk, 101);
		mem_free(&f.mem);
	}
	for (unsigned fast = 0; fast < 2; fast++) {
		struct fixture f;
		setup(&f, 0, fast, false);
		f.cpu.bus.fetch_wait = no_fetch_wait;
		model.ivram_fetch_wait = 100;
		f.cpu.pc = IVRAM_BASE;
		mem_write(&f.mem, IVRAM_BASE, 2, 0xdf00);
		mem_write(&f.mem, REG(0x1198), 4, IVRAM_BASE);
		c33_step(&f.cpu);
		check("fixed internal fetch latency also settles DMA before decode", f.cpu.ext[0], 0x1fff);
		check("fixed internal fetch uses its actual response boundary", f.last_data_clock, 100);
		check("fixed fetch latency is charged only once", f.cpu.clk, 101);
		mem_free(&f.mem);
	}
	model = saved;
}

static void multiword_fetch_order(void)
{
	struct model saved = model;
	model.iqb_first = model.dq_extra = model.dq_hit = model.dq_iram_extra = 0;
	model.iqb_word_gap = 40; /* SPI arrives before the fourth word can issue. */
	model.row_ports = model.row_change_extra = model.act_overlap = 0;
	model.write_post = model.wr_ticks = model.wr_rd_turn = model.write_recovery = 0;
	model.iq_lookahead = model.branch_bubble = 0;
	model.sdclk_half = 2;
	for (unsigned policy = 1; policy <= 2; policy++)
	for (unsigned fast = 0; fast < 2; fast++)
	for (unsigned generic = 0; generic < 2; generic++) {
		struct fixture f;
		struct sdramc s;
		model.iq_deferred = policy;
		setup(&f, 0, fast, false);
		sdramc_attach(&f.mem, &s);
		mem_write(&f.mem, REG(0x1604), 4, 0x1243);
		mem_write(&f.mem, REG(0x1608), 4, 0xfff);
		mem_write(&f.mem, REG(0x1610), 4, 0x8000000b);
		mem_write(&f.mem, REG(0x1600), 4, (1u << 4) | (1u << 2));
		f.cpu.bus.fetch_wait = generic ? NULL : sdramc_fetch_wait;
		f.cpu.bus.fetch_wait_ctx = &s;
		f.cpu.bus.wait = mem_wait;
		f.cpu.pc = DEST + 12;
		mem_write(&f.mem, f.cpu.pc, 2, 0xdf00);
		mem_write(&f.mem, REG(0x1198), 4, f.cpu.pc);
		sdramc_set_events(&s, &f.cpu.clk, settle, &f);
		sd_poll(&f.sd); dma_poll(&f.dma);
		c33_step(&f.cpu);
		check("SPI DMA preempts a blocked multi-word instruction fetch", f.cpu.ext[0], 0x1fff);
		check("code update lands before the demanded word is reissued", mem_read(&f.mem, DEST + 12, 2), 0xdfff);
		check("partially invalidated fill restarts after DMA", s.iq_word_reads, 7);
		check("word-boundary events run while the CPU is blocked", s.event_polls, 7);
		check("callback clock progress is not charged twice", f.cpu.clk, 119);
		check("only the final instruction retires", f.cpu.cycles, 1);
		mem_free(&f.mem);
	}
	model = saved;
}

int main(void)
{
	/* No fitted bus holds: this isolates event ordering from arbitration. */
	model.dma_async = 1;
	model.dma_extra = model.dma_bus_hold = model.dma_reg_hold = 0;
	model.dma_cpu_penalty = model.mmio_wait = model.itc_wait = 0;
	for (unsigned fast = 0; fast <= 1; fast++) {
		for (unsigned generic = 0; generic <= 1; generic++) {
			struct fixture f;
			setup(&f, 0x3010, fast, generic); /* ld.w r0,[r1] */
			sd_poll(&f.sd); dma_poll(&f.dma); /* main loop before step */
			c33_step(&f.cpu);
			check("load after a fetch wait sees the earlier SPI DMA write", f.cpu.r[0], 0xff);
			sd_poll(&f.sd); dma_poll(&f.dma); /* next main-loop boundary */
			check("the receive actually landed", mem_read(&f.mem, DEST, 4), 0xff);
			mem_free(&f.mem);

			setup(&f, 0x3c10, fast, generic); /* ld.w [r1],r0 */
			f.cpu.r[0] = 0x1234abcd;
			c33_step(&f.cpu);
			sd_poll(&f.sd); dma_poll(&f.dma);
			check("later CPU store is not overwritten by an overdue DMA event",
				mem_read(&f.mem, DEST, 4), 0x1234abcd);
			mem_free(&f.mem);
		}
	}
	struct fixture f;
	setup(&f, 0x3010, true, false);
	f.remap = true;
	f.cpu.r[1] = DEST + 0x1000000;
	mem_write(&f.mem, f.cpu.r[1], 4, 0xaaaaaaaa);
	/* Prime the direct-data cache before the callback changes decoding. */
	uint32_t base, len;
	f.cpu.data_ptr = mem_region(&f.mem, f.cpu.r[1], &base, &len);
	f.cpu.data_lo = base; f.cpu.data_hi = base + len;
	f.cpu.region_epoch_seen = f.mem.region_epoch;
	c33_step(&f.cpu);
	check("data cache follows a memory-map change made by a due event", f.cpu.r[0], 0xff);
	mem_free(&f.mem);
	address_phase();
	frontend_dma_order();
	instruction_fetch_order();
	multiword_fetch_order();
	return failures != 0;
}
