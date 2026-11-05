/* A slow instruction fetch is still a fetch, not an additional data read.
 * Tiny isolated instructions compare cached, rejected and absent mappings.
 */
#include <stdio.h>
#include <string.h>
#include "../src/c33.h"
#include "../src/model.h"
#include "../src/mem.h"

#define ENTRY SDRAM_BASE
#define DATA (SDRAM_BASE + 0x1000u)

struct fixture {
	unsigned mapped, fetches, reads, specialized, event_callbacks;
	bool zero_fetch_wait;
	bool advance_clock;
	uint64_t *clock;
	unsigned char code[2];
};

static unsigned failures;

static void check(const char *label, uint64_t got, uint64_t expected)
{
	if (got != expected) {
		printf("FAIL %s: got %llu, expected %llu\n", label,
		       (unsigned long long)got, (unsigned long long)expected);
		failures++;
	}
}

static uint32_t read_bus(void *ctx, uint32_t addr, unsigned size)
{
	struct fixture *f = ctx;
	return addr == ENTRY ? f->code[0] | (uint32_t)f->code[1] << 8 : 0x12345678;
}

static uint8_t *region(void *ctx, uint32_t addr, uint32_t *base, uint32_t *length)
{
	struct fixture *f = ctx;
	*base = ENTRY;
	*length = 2;
	return f->mapped && addr == ENTRY ? f->code : NULL;
}

static uint64_t response(struct fixture *f, uint64_t now, uint64_t elapsed)
{
	if (f->advance_clock)
		*f->clock = now + elapsed;
	return elapsed;
}

static uint64_t wait_bus(void *ctx, enum mem_access kind,
			 uint32_t addr, unsigned size, uint64_t now)
{
	struct fixture *f = ctx;
	if (kind == MEM_CPU_FETCH) {
		f->fetches++;
		return response(f, now, f->zero_fetch_wait ? 0 : 13);
	}
	f->reads++;
	return response(f, now, 17);
}

static uint64_t wait_fetch(void *ctx, uint32_t addr, uint64_t now)
{
	struct fixture *f = ctx;
	f->specialized++;
	return response(f, now, f->zero_fetch_wait ? 0 : 7);
}

static void before_data(void *ctx)
{
	((struct fixture *)ctx)->event_callbacks++;
}

int main(void)
{
	for (unsigned mapping = 0; mapping < 3; mapping++)
	for (unsigned specialized = 0; specialized < 2; specialized++)
	for (unsigned load = 0; load < 2; load++)
	for (unsigned zero = 0; zero < 2; zero++)
	for (unsigned advance = 0; advance < 2; advance++) {
		struct fixture f = {.mapped = mapping == 2, .zero_fetch_wait = zero, .advance_clock = advance};
		struct c33 cpu = {0};
		f.clock = &cpu.clk;
		uint16_t insn = load ? 0x3010 : 0x0000; /* ld.w r0,[r1] / nop */
		f.code[0] = insn;
		f.code[1] = insn >> 8;
		cpu.bus = (struct c33_bus){
			.read = read_bus, .ctx = &f,
			.region = mapping ? region : NULL,
			.wait = wait_bus,
			.fetch_wait = specialized ? wait_fetch : NULL, .fetch_wait_ctx = &f,
			.before_data = before_data, .before_data_ctx = &f,
		};
		c33_reset(&cpu, ENTRY);
		cpu.r[1] = DATA;
		c33_step(&cpu);
		printf("fetch mapping=%u specialized=%u memory-op=%u zero-wait=%u advance-clock=%u\n", mapping, specialized, load, zero, advance);
		check("one instruction fetch wait", f.fetches + f.specialized, 1);
		check("data wait only for the actual data operation", f.reads, load);
		check("events settle after fetch waits and around actual data waits",
		      f.event_callbacks, !zero + 2 * load);
		check("touch counter excludes instruction fetches", cpu.touches, load);
		check("fetch and data timing charged once each", cpu.clk,
		      (zero ? 0 : specialized ? 7 : 13) + 1 + load * 17);
		check("instruction semantics preserved", cpu.r[0], load ? 0x12345678 : 0);
		check("instruction retires once", cpu.cycles, 1);
	}
	printf("fetch accounting: %u failures\n", failures);
	return failures != 0;
}
