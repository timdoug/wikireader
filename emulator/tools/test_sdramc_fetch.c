/* Compare the queue-hit path with the unchanged general scheduler, including
 * all controller state and diagnostic counters after every transaction.
 * Include the implementation to reach the reference path without adding a
 * test-only entry point to the emulator's public interface.
 */
#include <stdio.h>
#include <string.h>

#include "../src/sdramc.c"
#include "../src/c33.h"

#define INI (REG_BASE + SDRAMC_BASE)
#define CTL (INI + 4)
#define REF (INI + 8)
#define APP (INI + 16)

struct bus_probe {
	struct mem *mem;
	uint64_t calls, last;
};

static void hold_bus(void *ctx, uint64_t now)
{
	struct bus_probe *p = ctx;
	p->calls++;
	p->last = now;
	if (p->mem->dma_bus_free < now + 7)
		p->mem->dma_bus_free = now + 7;
}

static uint32_t rng = 0x33f37c;

static unsigned random_value(void)
{
	rng = rng * 1664525u + 1013904223u;
	return rng >> 16;
}

static uint32_t read_nop(void *ctx, uint32_t addr, unsigned size)
{
	return 0;
}

static uint8_t *region_nop(void *ctx, uint32_t addr, uint32_t *base, uint32_t *len)
{
	static uint8_t nop[2];
	*base = SDRAM_BASE;
	*len = sizeof nop;
	return nop;
}

static uint64_t generic_wait(void *ctx, enum mem_access access,
			     uint32_t addr, unsigned size, uint64_t now)
{
	(*(unsigned *)ctx)++;
	return 13;
}

static uint64_t specialized_wait(void *ctx, uint32_t addr, uint64_t now)
{
	(*(unsigned *)ctx)++;
	return 7;
}

static bool test_fetch_dispatch(void)
{
	struct c33 cpu = {0};
	unsigned generic_calls = 0, specialized_calls = 0;
	cpu.bus.read = read_nop;
	cpu.bus.region = region_nop;
	cpu.bus.wait = generic_wait;
	cpu.bus.ctx = &generic_calls;
	c33_reset(&cpu, SDRAM_BASE);
	c33_step(&cpu);
	if (generic_calls != 1 || cpu.clk != 14)
		return false;
	cpu.bus.fetch_wait = specialized_wait;
	cpu.bus.fetch_wait_ctx = &specialized_calls;
	c33_reset(&cpu, SDRAM_BASE);
	c33_step(&cpu);
	return generic_calls == 1 && specialized_calls == 1 && cpu.clk == 8;
}

int main(void)
{
	if (!test_fetch_dispatch()) {
		puts("instruction-fetch callback dispatch/reset FAIL");
		return 1;
	}
	struct model defaults = model;
	unsigned comparisons = 0;
	static const unsigned lookahead[] = {0, 6, 17, 0xffffffffu};
	static const unsigned refresh[] = {0, 7, 140, 4095};
	static const unsigned gaps[] = {0, 1, 2, 4, 8, 1000, 65536};
	for (unsigned trial = 0; trial < 64; trial++) {
		struct mem fast_mem = {0}, ref_mem = {0};
		struct sdramc fast, ref;
		struct bus_probe fast_bus = {.mem = &fast_mem};
		struct bus_probe ref_bus = {.mem = &ref_mem};
		bool fast_write = false, ref_write = false;
		model = defaults;
		model.iq_lookahead = lookahead[trial % 4];
		model.iq_lookahead_seq = (trial >> 2) & 1;
		model.iq_row_evict = (trial >> 3) & 1;
		model.branch_bubble = trial % 3 ? 3 : 0;
		model.sdclk_half = trial & 1 ? 2 : 4;
		model.row_ports = (trial >> 1) & 1;
		model.bank_conflict = (trial >> 2) & 1;
		sdramc_attach(&fast_mem, &fast);
		sdramc_attach(&ref_mem, &ref);
		mem_set_timing(&ref_mem, sdramc_wait_slow, &ref);
		fast_mem.cpu_data_hook = ref_mem.cpu_data_hook = hold_bus;
		fast_mem.cpu_data_ctx = &fast_bus;
		ref_mem.cpu_data_ctx = &ref_bus;
		uint32_t ctl = 0x37e0 | (trial & 7);
		uint32_t refctl = refresh[trial % 4] |
			(trial & 8 ? SELEN | (127u << 16) : 0);
		uint32_t app = 0x8000000b | (trial & 16 ? DBF : 0);
		mem_write(&fast_mem, CTL, 4, ctl);
		mem_write(&ref_mem, CTL, 4, ctl);
		mem_write(&fast_mem, REF, 4, refctl);
		mem_write(&ref_mem, REF, 4, refctl);
		mem_write(&fast_mem, APP, 4, app);
		mem_write(&ref_mem, APP, 4, app);
		mem_write(&fast_mem, INI, 4, SDON | INIMRS);
		mem_write(&ref_mem, INI, 4, SDON | INIMRS);

		uint64_t now = 0;
		uint32_t pc = SDRAM_BASE;
		for (unsigned step = 0; step < 4096; step++) {
			now += gaps[random_value() % 7];
			/* Exercise the exact refresh edge and the clocks around it. */
			if (step % 31 == 0 && fast.next_refresh > 2 * now + 2)
				now = fast.next_refresh / 2 - 1 + step % 3;
			if (step % 113 == 0) {
				uint32_t val = step & 1 ? refctl : 0;
				mem_write(&fast_mem, REF, 4, val);
				mem_write(&ref_mem, REF, 4, val);
			}
			if (step % 127 == 0) {
				uint32_t val = step & 1 ? app : app & ~IQBEN;
				mem_write(&fast_mem, APP, 4, val);
				mem_write(&ref_mem, APP, 4, val);
			}
			if (step % 149 == 0) {
				uint32_t val = step & 1 ? SDON | INIMRS : 0;
				mem_write(&fast_mem, INI, 4, val);
				mem_write(&ref_mem, INI, 4, val);
			}
			if (step % 251 == 0) {
				sdramc_reset(&fast);
				sdramc_reset(&ref);
				mem_write(&fast_mem, CTL, 4, ctl);
				mem_write(&ref_mem, CTL, 4, ctl);
				mem_write(&fast_mem, APP, 4, app);
				mem_write(&ref_mem, APP, 4, app);
				mem_write(&fast_mem, INI, 4, SDON | INIMRS);
				mem_write(&ref_mem, INI, 4, SDON | INIMRS);
			}
			unsigned choice = random_value() % 20;
			enum mem_access kind = choice < 14 ? MEM_CPU_FETCH :
				(enum mem_access)(1 + random_value() % 4);
			unsigned size = kind == MEM_CPU_FETCH ? 2 : 1u << (random_value() % 3);
			pc = SDRAM_BASE + ((pc + 2 + 2 * (random_value() & 1)) & 63u);
			uint32_t addr = kind == MEM_CPU_FETCH ? pc :
				SDRAM_BASE + (random_value() % 8) * 0x400000u +
				(random_value() & 0x7feu);
			if (step % 53 == 0)
				addr = pc; /* CPU/DMA stores invalidate a live instruction line. */
			if (step % 71 == 0)
				addr = step & 1 ? IVRAM_BASE : 0xffffffffu;
			fast_mem.dma_bus_free = ref_mem.dma_bus_free =
				now + (step & 3 ? 0 : 20);
			wremu_cur_pc = step & 1 ? pc : IVRAM_BASE;
			wremu_fetch_restart = random_value() % 4 == 0;
			last_was_write = fast_write;
			uint64_t got = kind == MEM_CPU_FETCH ?
				sdramc_fetch_wait(&fast, addr, now) :
				mem_wait(&fast_mem, kind, addr, size, now);
			fast_write = last_was_write;
			unsigned fast_kind = current_kind;
			last_was_write = ref_write;
			uint64_t want = mem_wait(&ref_mem, kind, addr, size, now);
			ref_write = last_was_write;
			struct sdramc normalized;
			memcpy(&normalized, &ref, sizeof normalized);
			normalized.mem = fast.mem;
			comparisons++;
			if (got != want || fast_write != ref_write || fast_kind != current_kind ||
			    memcmp(&fast, &normalized, sizeof fast) ||
			    fast_mem.dma_bus_free != ref_mem.dma_bus_free ||
			    fast_mem.sdram_alias != ref_mem.sdram_alias ||
			    fast_mem.region_epoch != ref_mem.region_epoch ||
			    fast_bus.calls != ref_bus.calls || fast_bus.last != ref_bus.last) {
				printf("SDRAM fetch mismatch: trial %u step %u kind %u addr %08x "
				       "wait %llu/%llu\n", trial, step, kind, addr,
				       (unsigned long long)got, (unsigned long long)want);
				return 1;
			}
			now += got;
		}
	}
	model = defaults;
	printf("SDRAM fetch: %u wait/state/diagnostic/DMA-hook comparisons passed\n",
	       comparisons);
	return 0;
}
