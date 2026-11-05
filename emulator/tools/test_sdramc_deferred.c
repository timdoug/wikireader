/* Isolated controller requests, not a guest/firmware boot. Check the word
 * boundaries at which DMA may preempt an unfinished instruction fill. */
#include <stdio.h>
#include <string.h>
#include "../src/sdramc.c"

#define REG(off) (REG_BASE + SDRAMC_BASE + (off))
struct fixture {
    struct mem mem;
    struct sdramc s;
    uint64_t clock, deadline, dma_done;
    unsigned event_kind, callbacks, issued_at_event;
    bool event_seen;
};
static struct model defaults;
static unsigned checks, failures;

static void check(const char *name, uint64_t got, uint64_t expected)
{
    checks++;
    if (got != expected) {
        fprintf(stderr, "%s: got %llu, expected %llu\n", name,
                (unsigned long long)got, (unsigned long long)expected);
        failures++;
    }
}

static void configure(struct fixture *f)
{
    mem_write(&f->mem, REG(4), 4, 0x1243);
    mem_write(&f->mem, REG(8), 4, 0xfff);
    mem_write(&f->mem, REG(16), 4, 0x8000000b);
    mem_write(&f->mem, REG(0), 4, SDON | INIMRS);
}

static void setup(struct fixture *f)
{
    memset(f, 0, sizeof *f);
    model = defaults;
    model.iq_deferred = 1;
    model.iqb_first = model.dq_extra = model.dq_hit = 0;
    model.iqb_word_gap = 2;
    model.sdclk_half = 2;
    model.row_ports = model.act_overlap = model.row_change_extra = 0;
    model.write_post = model.wr_ticks = model.wr_rd_turn = model.write_recovery = 0;
    model.iq_lookahead = model.branch_bubble = model.dma_fetch_hold = 0;
    wremu_fetch_restart = false;
    wremu_cur_pc = SDRAM_BASE;
    sdramc_attach(&f->mem, &f->s);
    configure(f);
}

static uint64_t request(struct fixture *f, enum mem_access kind, uint32_t addr,
                        unsigned size, uint64_t now)
{
    return mem_wait(&f->mem, kind, addr, size, now);
}

static void preemption(void)
{
    for (unsigned policy = 1; policy <= 2; policy++)
    for (unsigned boundary = 4; boundary <= 5; boundary++) {
        struct fixture f;
        setup(&f);
        model.iq_deferred = policy;
        check("first instruction waits only for its first word",
              request(&f, MEM_CPU_FETCH, SDRAM_BASE, 2, 0), 4);
        unsigned slot = iq_slot(&f.s, SDRAM_BASE);
        check("one word has actually issued", f.s.iq[slot].issued, 2);
        check("later word has no invented completion", f.s.iq[slot].ready[2], UINT64_MAX);
        check("bus owns only the active word", f.s.bus_free, 10);
        uint64_t wait = request(&f, MEM_DMA_READ, SDRAM_BASE + 0x100, 4, boundary);
        check("DMA wins before the unissued instruction words", boundary + wait, 8);
        check("DMA does not force a complete IQB reservation", f.s.iq[slot].issued, 2);
        check("requested next word resumes after DMA",
              request(&f, MEM_CPU_FETCH, SDRAM_BASE + 4, 2, 9), 1);
        check("resumed word data follows DMA's completion", f.s.iq[slot].ready[2], 20);
        check("resumed fill has two words, not four", f.s.iq[slot].issued, 4);
    }
}

static void same_master_policy(void)
{
    struct fixture f;
    setup(&f);
    model.iq_deferred = 2;
    request(&f, MEM_CPU_FETCH, SDRAM_BASE, 2, 0);
    check("CPU miss waits for the same-master fill under policy two",
          5 + request(&f, MEM_CPU_READ, SDRAM_BASE + 0x100, 4, 5), 17);
    check("CPU miss drained only the already started fill", f.s.iq_word_reads, 4);
    setup(&f);
    model.iq_deferred = 2;
    request(&f, MEM_CPU_FETCH, SDRAM_BASE, 2, 0);
    f.s.dq[0].valid = true;
    f.s.dq[0].tag = SDRAM_BASE + 0x100;
    check("cached CPU data bypasses external fill arbitration",
          request(&f, MEM_CPU_READ, SDRAM_BASE + 0x100, 4, 4), 0);
    check("cache hit does not drain instruction words", f.s.iq_word_reads, 1);
}

static void background_and_cache(void)
{
    struct fixture f;
    setup(&f);
    request(&f, MEM_CPU_FETCH, SDRAM_BASE, 2, 0);
    check("CPU execution lets the background fill finish",
          request(&f, MEM_CPU_FETCH, SDRAM_BASE + 2, 2, 100), 0);
    unsigned first = iq_slot(&f.s, SDRAM_BASE);
    check("all four physical word reads finish", f.s.iq[first].issued, 8);
    check("background completion requires four reads", f.s.iq_word_reads, 4);
    for (unsigned i = 0; i < 8; i++)
        check("uncontended word timings follow consecutive READ phases",
              f.s.iq[first].ready[i], 8 + (i / 2) * 6 + (i & 1) * 2);
    request(&f, MEM_CPU_FETCH, SDRAM_BASE + 0x400, 2, 101);
    check("completed IQB data survives a physical row change",
          request(&f, MEM_CPU_FETCH, SDRAM_BASE, 2, 102), 0);
    check("both row-independent cache slots remain valid", f.s.iq[first].valid, true);
    check("long idle interval does not spin or invalidate cached data",
          request(&f, MEM_CPU_FETCH, SDRAM_BASE, 2, 1000000000000ull), 0);
}

static void speculative_replacement(void)
{
    struct fixture f;
    setup(&f);
    request(&f, MEM_CPU_FETCH, SDRAM_BASE, 2, 0);
    request(&f, MEM_CPU_FETCH, SDRAM_BASE + 16, 2, 50);
    request(&f, MEM_CPU_FETCH, SDRAM_BASE + 24, 2, 100);
    model.iq_lookahead = 6;
    request(&f, MEM_CPU_FETCH, SDRAM_BASE + 26, 2, 100);
    check("third line is requested without premature eviction", f.s.iq_prefetch_pending, true);
    check("loop target still occupies a valid slot", iq_slot(&f.s, SDRAM_BASE) < 2, true);
    check("no speculative word has started yet", f.s.iq_prefetches, 0);
    check("redirect can reuse the target before replacement starts",
          request(&f, MEM_CPU_FETCH, SDRAM_BASE, 2, 100), 0);
    check("redirect cancels only the unstarted speculative request", f.s.iq_prefetch_cancelled, 1);
    check("cancelled request reserves no external word", f.s.iq_prefetches, 0);
    check("cancelled prefetch no longer pending", f.s.iq_prefetch_pending, false);
}

static void coherence_and_reset(void)
{
    struct fixture f;
    setup(&f);
    request(&f, MEM_CPU_FETCH, SDRAM_BASE, 2, 0);
    unsigned slot = iq_slot(&f.s, SDRAM_BASE);
    request(&f, MEM_DMA_WRITE, SDRAM_BASE, 4, 1);
    check("reserving a DMA write does not invalidate instruction data", f.s.iq[slot].valid, true);
    /* This controller-only fixture has no backing RAM. Deliver the
     * acceptance notification normally emitted by mem_write. */
    f.mem.ram_written(f.mem.ram_written_ctx, SDRAM_BASE, 4);
    check("DMA write invalidates the partially filled instruction slot", f.s.iq[slot].valid, false);
    uint64_t occupied = f.s.bus_free;
    deferred_iq_advance(&f.s, 100);
    check("invalidated fill issues no later word", f.s.iq_word_reads, 1);
    check("invalidation preserves the issued external occupancy", f.s.bus_free, occupied);
    f.s.iq_prefetch_pending = true;
    mem_write(&f.mem, REG(16), 4, 0x8000000b);
    check("application reconfiguration cancels pending prefetch", f.s.iq_prefetch_pending, false);
    check("application reconfiguration drops cached slots", f.s.iq[slot].valid, false);
    sdramc_reset(&f.s);
    check("reset clears deferred scheduling state", f.s.iq_word_reads | f.s.iq_order, 0);
}

static void timeline_event(void *ctx)
{
    struct fixture *f = ctx;
    f->callbacks++;
    if (f->event_seen || f->clock < f->deadline)
        return;
    f->event_seen = true;
    if (f->event_kind == 1) {
        unsigned slot = iq_slot(&f->s, SDRAM_BASE);
        f->issued_at_event = f->s.iq[slot].issued;
        f->dma_done = f->deadline + request(f, MEM_DMA_READ,
                                           SDRAM_BASE + 0x100, 4, f->deadline);
    } else if (f->event_kind == 2) {
        sdramc_reset(&f->s);
        configure(f);
    } else if (f->event_kind == 3) {
        mem_write(&f->mem, REG(16), 4, 0x8000000a); /* IQB off, DQB remains */
    } else {
        mem_write(&f->mem, REG(0), 4, 0);
    }
}

static void timeline_boundaries(void)
{
    struct fixture f;
    setup(&f);
    mem_write(&f.mem, REG(16), 4, 0x8000000b | DBF);
    f.deadline = 3;
    f.event_kind = 1;
    sdramc_set_events(&f.s, &f.clock, timeline_event, &f);
    uint64_t wait = request(&f, MEM_CPU_FETCH, SDRAM_BASE + 12, 2, 0);
    check("event after a half-clock boundary cannot preempt an issued word",
          f.issued_at_event, 4);
    check("late DMA read waits for that word, then outranks the next", f.dma_done, 6);
    check("CPU response follows the intervening DMA phase", wait, 10);
    check("nested DMA timing does not recursively poll CPU events", f.callbacks, 4);
    for (unsigned kind = 2; kind <= 4; kind++) {
        setup(&f);
        f.deadline = 1;
        f.event_kind = kind;
        sdramc_set_events(&f.s, &f.clock, timeline_event, &f);
        wait = request(&f, MEM_CPU_FETCH, SDRAM_BASE + 12, 2, 0);
        check("controller changes during a word wait are observed", f.event_seen, true);
        check("reset/reconfiguration retains timeline clock wiring", f.s.event_clock == &f.clock, true);
        check("reset/reconfiguration retains event callback wiring", f.s.events == timeline_event, true);
        check("invalidated demand resumes with the configured interface", wait,
              kind == 2 ? 18 : kind == 3 ? 9 : 5);
        check("callback progress never exceeds the returned response", f.clock <= wait, true);
    }
}

int main(void)
{
    defaults = model;
    preemption();
    same_master_policy();
    background_and_cache();
    speculative_replacement();
    coherence_and_reset();
    timeline_boundaries();
    model = defaults;
    printf("Deferred SDRAM instruction fills: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
