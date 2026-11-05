/* Native device tests for shared SPI/DMA event ordering, source response
 * sampling, descriptor writeback, queue pressure and reset cancellation.
 * No guest or firmware is booted here; integration uses FLASH and Grifo. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/cmu.h"
#include "../src/dma.h"
#include "../src/itc.h"
#include "../src/model.h"
#include "../src/port.h"
#include "../src/sdcard.h"

#define REG(a) (REG_BASE + (a))
#define SOURCE (SDRAM_BASE + 0x1000u)
#define DEST (IVRAM_BASE + 0x100u)

struct waits { uint32_t desc; unsigned descriptor, source; uint32_t source_addr; };
static unsigned failures;

struct phase_trace {
    struct mem *mem;
    struct sdcard *sd;
    uint64_t *clock;
    uint32_t desc, dest, expected;
    bool to_spi;
    unsigned early;
};
static void observe_write(void *ctx, uint32_t addr, unsigned size)
{
    struct phase_trace *p = ctx;
    if (addr < p->desc || addr - p->desc >= 16) return;
    bool delivered = p->to_spi ? p->sd->busy : mem_read(p->mem, p->dest, 4) == p->expected;
    printf("PHASE descriptor-write offset=%u shared-clock=%llu destination-delivered=%u\n",
           addr - p->desc, (unsigned long long)*p->clock, delivered);
    if (!delivered) p->early++;
}


static uint64_t wait_source(void *ctx, enum mem_access kind, uint32_t addr,
			    unsigned size, uint64_t now)
{
	struct waits *w = ctx;
	if (kind != MEM_DMA_READ) return 0;
	if (addr == w->source_addr) return w->source;
	return addr >= w->desc && addr - w->desc < 16 ? w->descriptor : 0;
}

static void check(const char *label, uint32_t got, uint32_t want)
{
	printf("%-65s %s\n", label, got == want ? "ok" : "FAIL");
	if (got != want) {
		printf("  got %08x, expected %08x\n", got, want);
		failures++;
	}
}

static bool timestamp_read(void *ctx, uint32_t off, unsigned size,
                           uint32_t *value, bool writing)
{
    if (!writing) *value = (uint32_t)*(uint64_t *)ctx;
    return true;
}

static void run(unsigned desc_wait, unsigned src_wait, unsigned latency, bool to_spi, bool timestamp)
{
	struct mem m;
	struct cmu cmu;
	struct itc itc;
	struct port port;
	struct sdcard sd;
	struct dma dma;
	uint64_t clock = 0;
	uint32_t base = desc_wait ? SDRAM_BASE + 0x4000 : DSTRAM_BASE;
	uint32_t dest = to_spi ? REG(0x1704) : DEST;
	uint32_t source_addr = timestamp ? REG(0x1f00) : SOURCE;
    uint32_t expected = timestamp ? 5 + 4 * desc_wait + src_wait : 0x12345678;
    struct waits waits = {base + 0x240, desc_wait, src_wait, source_addr};
	assert(mem_init(&m));
	itc_attach(&m, &itc);
	cmu_attach(&m, &cmu);
	port_attach(&m, &port, &itc);
	assert(sd_attach(&m, &sd, NULL, &port, NULL, true));
	dma_attach(&m, &dma, &itc, &cmu, &sd);
	dma_set_clock(&dma, &clock);
	sd_set_clock(&sd, &clock);
	mem_set_timing(&m, wait_source, &waits);
	model.idma_extra = latency;
    if (timestamp) mem_add_mmio(&m, "timestamp source", 0x1f00, 4, timestamp_read, &clock);
    else mem_write(&m, SOURCE, 4, 0x12345678);
	mem_write(&m, DEST, 4, 0xcccccccc);
	if (to_spi)
		mem_write(&m, REG(0x1708), 4, (31u << 10) | 3u); /* 32-bit master */
	mem_write(&m, REG(0x1100), 4, base);
	mem_write(&m, waits.desc, 4, 2u << 16); /* word, fixed source/destination */
	mem_write(&m, waits.desc + 4, 4, 1);
	mem_write(&m, waits.desc + 8, 4, source_addr);
	mem_write(&m, waits.desc + 12, 4, dest);
	mem_write(&m, REG(0x1105), 1, 1);

    struct phase_trace trace = {&m, &sd, &clock, waits.desc, dest, expected, to_spi, 0};
    m.ram_written = observe_write;
    m.ram_written_ctx = &trace;
	mem_write(&m, REG(0x1104), 1, 0x80 | 0x24); /* single software invocation */
	assert(dma.qn == 1 && clock == 0);
	/* Four descriptor-read phases, one source-read phase, one write phase.
	 * Waits are in addition to those documented bus phases. */
	uint64_t earliest = 4 * (1 + desc_wait) + (1 + src_wait) + 1;
	uint64_t due = earliest > latency ? earliest : latency;
	clock = due - 1;
	dma_poll(&dma);
	if (to_spi)
		check("SPI cannot start transmitting before IDMA source reads finish", sd.busy, false);
	else
		check("IDMA destination stays unchanged before all read/write phases", mem_read(&m, DEST, 4), 0xcccccccc);
	clock = due;
	dma_poll(&dma);
	if (to_spi)
		check("SPI starts when IDMA can legally supply its word", sd.busy, true);
	else
		check("IDMA destination arrives at the legal deadline", mem_read(&m, DEST, 4), expected);

    check("descriptor count is unchanged when payload is accepted", mem_read(&m, waits.desc + 4, 4), 1);
    for (unsigned i = 1; i <= 4; i++) {
        clock = due + i;
        dma_poll(&dma);
        check("descriptor count changes on its own writeback phase",
              mem_read(&m, waits.desc + 4, 4), i < 2 ? 1 : 0);
        check("IDMA remains active until the fourth descriptor write", dma.qn, i < 4 ? 1 : 0);
    }
    check("complete IDMA uses ten actual bus phases", dma.bus_cycles, 10);
    check("complete IDMA records one payload", dma.idma_transfers, 1);
	check("descriptor writeback follows payload acceptance (manual II.2.4.1)", trace.early, 0);
	mem_free(&m);
}


struct queued_fixture {
    struct mem mem;
    struct cmu cmu;
    struct itc itc;
    struct port port;
    struct sdcard sd;
    struct dma dma;
    struct waits waits;
    uint64_t clock;
};
static void queued_setup(struct queued_fixture *f, unsigned source_wait)
{
    memset(f, 0, sizeof *f);
    assert(mem_init(&f->mem));
    itc_attach(&f->mem, &f->itc);
    cmu_attach(&f->mem, &f->cmu);
    port_attach(&f->mem, &f->port, &f->itc);
    assert(sd_attach(&f->mem, &f->sd, NULL, &f->port, NULL, true));
    dma_attach(&f->mem, &f->dma, &f->itc, &f->cmu, &f->sd);
    sd_set_clock(&f->sd, &f->clock);
    dma_set_clock(&f->dma, &f->clock);
    f->waits = (struct waits){DSTRAM_BASE + 0x240, 0, source_wait, SOURCE};
    mem_set_timing(&f->mem, wait_source, &f->waits);
    model.idma_extra = 38;
    mem_write(&f->mem, SOURCE, 4, 0x12345678);
    mem_write(&f->mem, DEST, 4, 0xcccccccc);
    mem_write(&f->mem, REG(0x1100), 4, DSTRAM_BASE);
    mem_write(&f->mem, f->waits.desc, 4, 2u << 16);
    mem_write(&f->mem, f->waits.desc + 4, 4, 1);
    mem_write(&f->mem, f->waits.desc + 8, 4, SOURCE);
    mem_write(&f->mem, f->waits.desc + 12, 4, DEST);
    mem_write(&f->mem, REG(0x1105), 1, 1);
}
static void select_pattern(struct queued_fixture *f)
{
    f->port.reg[OFF_P3D] &= (uint8_t)~(1u << 2);
    f->port.reg[OFF_P5D] &= (uint8_t)~(1u << CS_SDCARD_BIT);
    sd_poll(&f->sd);
    f->sd.resp[0] = 0x11; f->sd.resp[1] = 0x22; f->sd.resp[2] = 0x33;
    f->sd.resp_pos = 0; f->sd.resp_len = 3;
}
static void queued_transfers(bool late)
{
    struct queued_fixture f;
    queued_setup(&f, 0);
    mem_write(&f.mem, SOURCE + 4, 4, 0x87654321);
    mem_write(&f.mem, DEST + 4, 4, 0xcccccccc);
    mem_write(&f.mem, f.waits.desc, 4, (2u << 16) | (3u << 12) | (3u << 8));
    mem_write(&f.mem, f.waits.desc + 4, 4, 2);
    mem_write(&f.mem, REG(0x1104), 1, 0xa4);
    mem_write(&f.mem, REG(0x1104), 1, 0xa4);
    if (late) { f.clock = 1000; dma_poll(&f.dma); }
    else for (f.clock = 0; f.clock <= 1000; f.clock++) dma_poll(&f.dma);
    check("queued IDMA first payload", mem_read(&f.mem, DEST, 4), 0x12345678);
    check("queued IDMA second payload uses updated descriptor", mem_read(&f.mem, DEST + 4, 4), 0x87654321);
    check("queued IDMA counter decremented twice", mem_read(&f.mem, f.waits.desc + 4, 4), 0);
    check("queued IDMA advances source twice", mem_read(&f.mem, f.waits.desc + 8, 4), SOURCE + 8);
    check("queued IDMA advances destination twice", mem_read(&f.mem, f.waits.desc + 12, 4), DEST + 8);
    check("queued IDMA completes both transactions", f.dma.idma_transfers, 2);
    check("queued IDMA preserves all twenty bus phases", f.dma.bus_cycles, 20);
    check("queued IDMA drains completely", f.dma.qn, 0);
    mem_free(&f.mem);
}
static void cancelled_transfers(void)
{
    static const unsigned cancel[] = {0, 1, 4, 50, 105, 106, 108, 110};
    for (unsigned t = 0; t < sizeof cancel / sizeof *cancel; t++) {
        struct queued_fixture f;
        queued_setup(&f, 100);
        mem_write(&f.mem, REG(0x1104), 1, 0xa4);
        f.clock = cancel[t]; dma_poll(&f.dma);
        uint32_t payload = mem_read(&f.mem, DEST, 4), words[4];
        for (unsigned i = 0; i < 4; i++) words[i] = mem_read(&f.mem, f.waits.desc + 4 * i, 4);
        dma_reset(&f.dma);
        f.clock = 1000; dma_poll(&f.dma);
        check("reset cancels the pending payload", mem_read(&f.mem, DEST, 4), payload);
        for (unsigned i = 0; i < 4; i++)
            check("reset cancels remaining descriptor writes", mem_read(&f.mem, f.waits.desc + 4 * i, 4), words[i]);
        check("reset leaves no pending transaction", f.dma.qn, 0);
        check("reset preserves the shared clock connection", f.dma.clock == &f.clock, true);
        check("reset preserves the SPI timeline connection", f.dma.sd == &f.sd, true);
        sd_reset(&f.sd);
        check("SPI reset preserves event settlement wiring",
              f.sd.before_event != NULL && f.sd.before_event_ctx == &f.dma, true);
        mem_free(&f.mem);
    }
}
static uint64_t slow_writeback(void *ctx, enum mem_access kind, uint32_t addr,
                               unsigned size, uint64_t now)
{
    struct waits *w = ctx;
    if (kind == MEM_DMA_WRITE && addr == w->desc + 12) return 40;
    return wait_source(ctx, kind, addr, size, now);
}
static void receive_during_writeback(bool late)
{
    struct queued_fixture f;
    queued_setup(&f, 0);
    mem_set_timing(&f.mem, slow_writeback, &f.waits);
    mem_write(&f.mem, f.waits.desc, 4, 0); /* byte, fixed source/destination */
    mem_write(&f.mem, f.waits.desc + 4, 4, 2);
    mem_write(&f.mem, f.waits.desc + 12, 4, REG(0x1704));
    mem_write(&f.mem, SOURCE, 4, 0xffffffff);
    f.port.reg[OFF_P3D] |= 1u << 3;
    f.port.reg[OFF_P5D] |= 1u << CS_SDCARD_BIT;
    mem_write(&f.mem, REG(0x1708), 4, (7u << 10) | 15u);
    mem_write(&f.mem, REG(0x1710), 4, 0);
    mem_write(&f.mem, REG(0x119c), 2, 1);
    mem_write(&f.mem, REG(0x1150), 2, 3);
    mem_write(&f.mem, REG(0x1152), 2, 0x8000);
    mem_write(&f.mem, REG(0x115a), 2, 0x3000);
    mem_write(&f.mem, REG(0x1194), 4, REG(0x1700));
    mem_write(&f.mem, REG(0x1198), 4, DEST + 0x100);
    mem_write(&f.mem, REG(0x0299), 1, 0x90);
    mem_write(&f.mem, REG(0x115c), 2, 1);
    mem_write(&f.mem, REG(0x029b), 1, 0x10);
    mem_write(&f.mem, REG(0x029c), 1, 0x10);
    select_pattern(&f);
    mem_write(&f.mem, REG(0x1704), 4, 0xff);
    if (late) {
        f.clock = 1000;
        for (unsigned i = 0; i < 6; i++) { sd_poll(&f.sd); dma_poll(&f.dma); }
    } else {
        for (f.clock = 0; f.clock <= 1000; f.clock++) { sd_poll(&f.sd); dma_poll(&f.dma); }
    }
    printf("receive-during-writeback late=%u\n", late);
    check("three SPI characters complete", f.sd.xfers, 3);
    check("new RX cause survives older IDMA writeback", f.dma.idma_transfers, 2);
    check("all three RX payloads reach memory", f.dma.hsdma_transfers, 3);
    check("batched event processing preserves RX data", f.sd.overflows, 0);
    check("late polling preserves the three distinct RX bytes", mem_read(&f.mem, DEST + 0x100, 4), 0x00332211);
    check("receive pipeline drains completely", f.dma.qn, 0);
    check("terminal IDMA disables its hardware channel", f.itc.reg[0x9c] & 0x10, 0);
    mem_free(&f.mem);
}


struct wire_trace {
    sd_dma_event_fn underlying;
    void *ctx;
    uint64_t *clock, receive[4];
    unsigned count;
};
static void wire_event(void *ctx, unsigned requests)
{
    struct wire_trace *w = ctx;
    if (requests & SPI_DMA_RX) {
        if (w->count < 4) w->receive[w->count] = *w->clock;
        w->count++;
    }
    w->underlying(w->ctx, requests);
}
static void buffered_spi_late_poll(void)
{
    struct model saved = model;
    model.dma_extra = model.spi_wait_extra = 0;
    struct queued_fixture f;
    queued_setup(&f, 0);
    mem_write(&f.mem, SOURCE, 4, 0xffffffff);
    f.port.reg[OFF_P3D] |= 1u << 3;
    f.port.reg[OFF_P5D] |= 1u << CS_SDCARD_BIT;
    mem_write(&f.mem, REG(0x1708), 4, (7u << 10) | 15u);
    mem_write(&f.mem, REG(0x1710), 4, 0);
    mem_write(&f.mem, REG(0x119c), 2, 1);
    mem_write(&f.mem, REG(0x1150), 2, 3);
    mem_write(&f.mem, REG(0x1152), 2, 0x8000);
    mem_write(&f.mem, REG(0x115a), 2, 0x3000);
    mem_write(&f.mem, REG(0x1194), 4, REG(0x1700));
    mem_write(&f.mem, REG(0x1198), 4, DEST + 0x100);
    mem_write(&f.mem, REG(0x1140), 2, 2);
    mem_write(&f.mem, REG(0x1142), 2, 0x8000);
    mem_write(&f.mem, REG(0x1184), 4, SOURCE);
    mem_write(&f.mem, REG(0x1188), 4, REG(0x1704));
    mem_write(&f.mem, REG(0x0299), 1, 0x99);
    mem_write(&f.mem, REG(0x115c), 2, 1);
    mem_write(&f.mem, REG(0x114c), 2, 1);
    struct wire_trace trace = {f.sd.dma_event, f.sd.dma_ctx, &f.clock, {0}, 0};
    sd_set_dma_event(&f.sd, wire_event, &trace);
    mem_write(&f.mem, REG(0x1704), 4, 0xff);
    f.clock = 4; dma_poll(&f.dma); /* Next TX word buffered before the first RX. */
    f.clock = 1000; sd_poll(&f.sd); dma_poll(&f.dma);
    check("buffered late-poll stream receives all three characters", trace.count, 3);
    check("first wire response remains on its physical deadline", trace.receive[0], 32);
    check("buffered second wire response includes the SPI wait", trace.receive[1], 68);
    check("late TX processing cannot start the third character in the past", trace.receive[2], 104);
    mem_free(&f.mem);
    model = saved;
}


static void phased_receive_before_spi_edge(void)
{
    struct model saved = model;
    model.dma_extra = model.spi_wait_extra = 0;
    struct queued_fixture f;
    queued_setup(&f, 0);
    f.port.reg[OFF_P3D] |= 1u << 3;
    f.port.reg[OFF_P5D] |= 1u << CS_SDCARD_BIT;
    mem_write(&f.mem, SOURCE, 4, 0xffffffff);
    mem_write(&f.mem, f.waits.desc, 4, 3u << 8); /* byte RX, advancing destination */
    mem_write(&f.mem, f.waits.desc + 4, 4, 3);
    mem_write(&f.mem, f.waits.desc + 8, 4, REG(0x1700));
    mem_write(&f.mem, f.waits.desc + 12, 4, DEST);
    mem_write(&f.mem, REG(0x1708), 4, (7u << 10) | 15u);
    mem_write(&f.mem, REG(0x1710), 4, 0);
    mem_write(&f.mem, REG(0x119c), 2, 1);
    mem_write(&f.mem, REG(0x1140), 2, 2);
    mem_write(&f.mem, REG(0x1142), 2, 0x8000);
    mem_write(&f.mem, REG(0x1184), 4, SOURCE);
    mem_write(&f.mem, REG(0x1188), 4, REG(0x1704));
    mem_write(&f.mem, REG(0x0299), 1, 0x09);
    mem_write(&f.mem, REG(0x114c), 2, 1);
    mem_write(&f.mem, REG(0x029b), 1, 0x10);
    mem_write(&f.mem, REG(0x029c), 1, 0x10);
    select_pattern(&f);
    mem_write(&f.mem, REG(0x1704), 4, 0xff);
    f.clock = 4; dma_poll(&f.dma);
    f.clock = 1000; sd_poll(&f.sd); dma_poll(&f.dma);
    check("phased RX consumes each byte before the following SPI edge", f.sd.overflows, 0);
    check("phased RX observes all three hardware triggers", f.dma.idma_transfers, 3);
    check("phased RX stream finishes all three wire characters", f.sd.xfers, 3);
    check("phased RX stream delivers its final payload", mem_read(&f.mem, DEST, 4), 0xcc332211);
    mem_free(&f.mem);
    model = saved;
}


static void pending_request_after_queue_pressure(void)
{
    struct model saved = model;
    model.dma_extra = 200;
    model.spi_wait_extra = 0;
    struct queued_fixture f;
    queued_setup(&f, 0);
    f.port.reg[OFF_P3D] |= 1u << 3;
    f.port.reg[OFF_P5D] |= 1u << CS_SDCARD_BIT;
    mem_write(&f.mem, REG(0x1708), 4, (7u << 10) | 15u);
    mem_write(&f.mem, REG(0x1710), 4, 0);
    mem_write(&f.mem, REG(0x119c), 2, 1);
    mem_write(&f.mem, REG(0x1150), 2, 4);
    mem_write(&f.mem, REG(0x1152), 2, 0x8000);
    mem_write(&f.mem, REG(0x115a), 2, 0x3000);
    mem_write(&f.mem, REG(0x1194), 4, REG(0x1700));
    mem_write(&f.mem, REG(0x1198), 4, DEST + 0x100);
    mem_write(&f.mem, REG(0x0299), 1, 0x90);
    mem_write(&f.mem, REG(0x115c), 2, 1);
    for (unsigned i = 0; i < 4; i++) {
        mem_write(&f.mem, REG(0x1704), 4, 0xff);
        while (f.sd.xfers <= i) {
            f.clock = f.sd.deadline;
            sd_poll(&f.sd); dma_poll(&f.dma);
        }
    }
    uint64_t before = f.clock;
    mem_write(&f.mem, REG(0x029b), 1, 0x10);
    mem_write(&f.mem, REG(0x029c), 1, 0x10);
    mem_write(&f.mem, REG(0x1105), 1, 1); /* attempt the latched RX request */
    check("full queue does not run IDMA synchronously on the CPU clock", f.clock, before);
    check("four HSDMA payloads occupy the pending queue", f.dma.qn, 4);
    check("blocked hardware IDMA exposes no early payload", mem_read(&f.mem, DEST, 4), 0xcccccccc);
    f.clock = 1000; dma_poll(&f.dma);
    check("freeing a queue slot retries the pending hardware IDMA", f.dma.idma_transfers, 1);
    check("all HSDMA payloads complete under queue pressure", f.dma.hsdma_transfers, 4);
    check("retried hardware IDMA delivers its source", mem_read(&f.mem, DEST, 4), 0x12345678);
    check("pressure pipeline drains completely", f.dma.qn, 0);
    check("retried terminal IDMA disables its channel", f.itc.reg[0x9c] & 0x10, 0);
    mem_free(&f.mem);
    model = saved;
}
static void gate_change_before_wire_edge(void)
{
    struct model saved = model;
    struct queued_fixture f;
    queued_setup(&f, 0);
    model.idma_extra = 0;
    sd_set_cmu(&f.sd, &f.cmu);
    uint32_t gates = mem_read(&f.mem, REG(0x1b04), 4);
    mem_write(&f.mem, REG(0x1b24), 4, 0x96);
    mem_write(&f.mem, SOURCE, 4, gates & ~(1u << 6));
    mem_write(&f.mem, f.waits.desc + 12, 4, REG(0x1b04));
    mem_write(&f.mem, REG(0x1104), 1, 0xa4);
    f.port.reg[OFF_P3D] |= 1u << 3;
    f.port.reg[OFF_P5D] |= 1u << CS_SDCARD_BIT;
    mem_write(&f.mem, REG(0x1708), 4, (7u << 10) | 15u);
    mem_write(&f.mem, REG(0x1704), 4, 0xff);
    f.clock = 100; sd_poll(&f.sd); dma_poll(&f.dma);
    check("DMA gate change is settled before the pending SPI edge", cmu_spi_enabled(&f.cmu), false);
    check("unclocked SPI does not complete the cancelled edge", f.sd.xfers, 0);
    check("wire-edge settlement preserves the caller's observed clock", f.clock, 100);
    mem_free(&f.mem);
    model = saved;
}


struct timestamp_counter { uint64_t *clock; unsigned reads; uint64_t sampled; };
static bool counted_timestamp(void *ctx, uint32_t off, unsigned size, uint32_t *value, bool writing)
{
    struct timestamp_counter *c = ctx;
    if (!writing) { c->reads++; c->sampled = *c->clock; *value = (uint32_t)*c->clock; }
    return true;
}
static void hsdma_source_response(unsigned source_wait, bool cpu_hold, bool cancel)
{
    struct model saved = model;
    model.dma_extra = 0;
    model.dma_cpu_penalty = cpu_hold ? 15 : 0;
    struct queued_fixture f;
    queued_setup(&f, source_wait);
    f.waits.source_addr = REG(0x1f00);
    struct timestamp_counter counter = {&f.clock, 0, 0};
    mem_add_mmio(&f.mem, "response timestamp", 0x1f00, 4, counted_timestamp, &counter);
    f.port.reg[OFF_P3D] |= 1u << 3;
    f.port.reg[OFF_P5D] |= 1u << CS_SDCARD_BIT;
    mem_write(&f.mem, REG(0x1708), 4, (7u << 10) | 15u);
    mem_write(&f.mem, REG(0x1710), 4, 0);
    mem_write(&f.mem, REG(0x119c), 2, 1);
    mem_write(&f.mem, REG(0x1150), 2, 1);
    mem_write(&f.mem, REG(0x1152), 2, 0x8000);
    mem_write(&f.mem, REG(0x1192), 2, 1); /* word transfer */
    mem_write(&f.mem, REG(0x1194), 4, REG(0x1f00));
    mem_write(&f.mem, REG(0x1198), 4, DEST);
    mem_write(&f.mem, REG(0x0299), 1, 0x90);
    mem_write(&f.mem, REG(0x115c), 2, 1);
    mem_write(&f.mem, REG(0x1704), 4, 0xff);
    f.clock = 32; sd_poll(&f.sd); dma_poll(&f.dma);
    check("HSDMA does not sample MMIO when its source request starts", counter.reads, 0);
    if (cpu_hold) f.mem.cpu_data_hook(f.mem.cpu_data_ctx, 32);
    if (cancel) {
        f.clock = 50; dma_poll(&f.dma); dma_reset(&f.dma);
        f.clock = 1000; dma_poll(&f.dma);
        check("reset cancels HSDMA source MMIO effects", counter.reads, 0);
        check("reset cancels HSDMA destination payload", mem_read(&f.mem, DEST, 4), 0xcccccccc);
    } else {
        unsigned response = 33 + source_wait;
        f.clock = response - 1; dma_poll(&f.dma);
        check("HSDMA source side effects wait for the full read latency", counter.reads, 0);
        f.clock = response; dma_poll(&f.dma);
        check("HSDMA source is sampled exactly once", counter.reads, 1);
        check("CPU destination hold cannot delay a reserved source response", counter.sampled, response);
        check("HSDMA payload still waits for its write phase", mem_read(&f.mem, DEST, 4), 0xcccccccc);
        unsigned accepted = cpu_hold ? 47 : response + 1;
        f.clock = accepted; dma_poll(&f.dma);
        check("HSDMA delivers the value sampled at the read response", mem_read(&f.mem, DEST, 4), response);
        check("HSDMA transaction completes after destination acceptance", f.dma.qn, 0);
    }
    mem_free(&f.mem);
    model = saved;
}

int main(void)
{
	model.dma_async = 1;
	model.dma_bus_hold = model.dma_mem_extra = 0;
	run(0, 0, 38, false, false);   /* existing calibrated path retains its allowance */
	run(0, 0, 0, false, false);    /* no allowance still needs the actual bus phases */
	run(0, 100, 38, false, false); /* slow source */
	run(100, 0, 38, false, false); /* external descriptor table */
	run(20, 40, 38, false, false); /* both forms of memory latency */
	run(0, 100, 38, true, false);  /* premature data would start the actual SPI wire */
	run(100, 0, 38, true, false);
    run(0, 100, 38, false, true); /* MMIO sampled at the actual response edge */
    queued_transfers(false); queued_transfers(true);
    cancelled_transfers();
    receive_during_writeback(false); receive_during_writeback(true);
    buffered_spi_late_poll();
    phased_receive_before_spi_edge();
    pending_request_after_queue_pressure();
    gate_change_before_wire_edge();
    hsdma_source_response(100, false, false);
    hsdma_source_response(0, true, false);
    hsdma_source_response(100, false, true);
	return failures != 0;
}
