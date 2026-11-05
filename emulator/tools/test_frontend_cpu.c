/* Isolated CPU instructions compare architectural behavior with and without
 * the experimental frontend. Firmware qualification still boots Grifo. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../src/c33.h"
#include "../src/model.h"

#define ENTRY 0x100u
#define TTBR 0x400u
#define HANDLER 0x600u

struct fixture { struct mem mem; struct c33 cpu; bool advance_clock; };
static unsigned checks;

static void setup(struct fixture *f)
{
    assert(mem_init(&f->mem));
    memset(&f->cpu, 0, sizeof f->cpu);
    f->cpu.bus = (struct c33_bus){.ctx = &f->mem, .read = mem_read, .write = mem_write,
        .wait = mem_wait,
        .region = (uint8_t *(*)(void *, uint32_t, uint32_t *, uint32_t *))mem_region};
    f->cpu.region_epoch = &f->mem.region_epoch;
    c33_reset(&f->cpu, ENTRY);
    f->cpu.sr[SR_TTBR] = TTBR;
    f->cpu.sr[SR_SP] = 0x1800;
    f->cpu.sp_initialized = true;
    f->cpu.r[1] = 0x800;
    f->cpu.r[4] = 13;
    f->cpu.r[5] = 0x12345678;
    for (unsigned vector = 0; vector < 64; vector++)
        mem_write(&f->mem, TTBR + 4 * vector, 4, HANDLER);
    mem_write(&f->mem, 0x800, 4, 13);
}

static void same(struct fixture *a, struct fixture *b)
{
    struct c33 *x = &a->cpu, *y = &b->cpu;
    assert(!memcmp(x->r, y->r, sizeof x->r));
    assert(!memcmp(x->sr, y->sr, sizeof x->sr));
    assert(!memcmp(x->ext, y->ext, sizeof x->ext));
    assert(!memcmp(x->ext_dropped, y->ext_dropped, sizeof x->ext_dropped));
    assert(x->pc == y->pc && x->n_ext == y->n_ext && x->cycles == y->cycles);
    assert(x->delay_pending == y->delay_pending && x->delay_target == y->delay_target);
    assert(x->halted == y->halted && x->sleeping == y->sleeping && x->debug_mode == y->debug_mode);
    assert(x->irq_pending == y->irq_pending && x->irqs_taken == y->irqs_taken && x->nmis_taken == y->nmis_taken);
    assert(x->access_fault == y->access_fault && x->misaligned_hits == y->misaligned_hits);
    assert(!memcmp(a->mem.a0ram, b->mem.a0ram, A0RAM_SIZE));
    checks++;
}

static void step(struct fixture *a, struct fixture *b)
{
    model.local_frontend = 0;
    c33_step(&a->cpu);
    model.local_frontend = 1;
    c33_step(&b->cpu);
    same(a, b);
}

static void program(struct fixture *a, struct fixture *b, const uint16_t *words, unsigned count)
{
    for (unsigned i = 0; i < count; i++) {
        mem_write(&a->mem, ENTRY + 2 * i, 2, words[i]);
        mem_write(&b->mem, ENTRY + 2 * i, 2, words[i]);
    }
}

static uint64_t progressing_wait(void *ctx, enum mem_access access,
                                uint32_t addr, unsigned size, uint64_t now)
{
    struct fixture *f = ctx; /* mem is the first member, as in bus.ctx. */
    uint64_t delay = access == MEM_CPU_FETCH ? 0 : 17;
    if (f->advance_clock)
        f->cpu.clk = now + delay;
    return delay;
}

static void clock_progress(void)
{
    static const uint16_t instructions[] = {0x3010, 0x3c10, 0xb010};
    model.local_frontend = 1;
    for (unsigned i = 0; i < sizeof instructions / sizeof *instructions; i++) {
        struct fixture a, b;
        setup(&a); setup(&b);
        program(&a, &b, &instructions[i], 1);
        a.advance_clock = false;
        b.advance_clock = true;
        a.cpu.bus.wait = b.cpu.bus.wait = progressing_wait;
        c33_step(&a.cpu); c33_step(&b.cpu);
        same(&a, &b);
        assert(a.cpu.frontend_valid && b.cpu.frontend_valid);
        assert(a.cpu.clk == b.cpu.clk);
        mem_free(&a.mem); mem_free(&b.mem);
    }
}

int main(void)
{
    struct model saved = model;
    model.local_frontend_depth = 8;
    static const uint16_t ordinary[] = {
        0x0000, 0x2e54, 0x3a54, 0x2814, 0x3014, 0x3c14,
        0x3114, 0x3d14, 0x1801, 0xaa54, 0x9254, 0x0093, 0x00d3
    };
    uint32_t random = 0x33e07;
    for (unsigned seed = 0; seed < 32; seed++) {
        struct fixture a, b;
        setup(&a); setup(&b);
        uint16_t words[128];
        unsigned count = 0;
        while (count < 100) {
            random = random * 1664525u + 1013904223u;
            if ((random & 7) == 0 && count < 96) {
                words[count++] = 0xc000;
                words[count++] = 0xc000;
                words[count++] = 0x6c54; /* extended ld.w r4,5 */
            } else if ((random & 7) == 1 && count < 96) {
                words[count++] = 0xc007;
                words[count++] = 0x73e4; /* extended AND */
            } else words[count++] = ordinary[(random >> 8) % (sizeof ordinary / sizeof *ordinary)];
        }
        program(&a, &b, words, count);
        for (unsigned i = 0; i < 120; i++) {
            if (i == 10) { /* direct/host patches must also remain coherent */
                mem_write(&a.mem, a.cpu.pc, 2, 0x6c15);
                mem_write(&b.mem, b.cpu.pc, 2, 0x6c15);
            }
            if (i == 30) {
                mem_set_ivram_area0(&a.mem, true);
                mem_set_ivram_area0(&b.mem, true);
            }
            step(&a, &b);
        }
        mem_free(&a.mem); mem_free(&b.mem);
    }
    for (unsigned test = 0; test < 4; test++) {
        struct fixture a, b;
        setup(&a); setup(&b);
        if (test == 0) {
            const uint16_t words[] = {0xc200, 0xc353, 0x6ec0, 0};
            program(&a, &b, words, 4);
            a.cpu.sr[SR_PSR] = b.cpu.sr[SR_PSR] = PSR_IE;
            step(&a, &b);
            c33_raise_irq(&a.cpu, 0x20, 3); c33_raise_irq(&b.cpu, 0x20, 3);
            step(&a, &b); step(&a, &b);
            assert(b.cpu.r[0] == 0x1000d4ec && !b.cpu.irqs_taken);
            step(&a, &b);
            assert(b.cpu.irqs_taken == 1 && b.cpu.pc == HANDLER + 2);
        } else if (test == 1) {
            const uint16_t words[] = {0xc000, 0xc000, 0xc000, 0};
            program(&a, &b, words, 4);
            step(&a, &b); step(&a, &b); step(&a, &b);
            assert(b.cpu.pc == HANDLER && !b.cpu.n_ext);
        } else if (test == 2) {
            const uint16_t words[] = {0x3814, 0, 0}; /* aligned halfword code patch */
            program(&a, &b, words, 3);
            a.cpu.r[1] = b.cpu.r[1] = ENTRY + 2;
            a.cpu.r[4] = b.cpu.r[4] = 0x6c15;
            step(&a, &b); step(&a, &b);
            assert(b.cpu.r[5] == 1);
        } else {
            const uint16_t words[] = {0x3014, 0};
            program(&a, &b, words, 2);
            a.cpu.r[1] = b.cpu.r[1] = 0x801;
            step(&a, &b);
            assert(b.cpu.access_fault && b.cpu.pc == HANDLER);
        }
        model.local_frontend = 1;
        c33_reset(&b.cpu, ENTRY);
        assert(!b.cpu.frontend_valid && !b.cpu.frontend_step && !b.cpu.frontend.raw_count);
        assert(b.cpu.bus.region && b.cpu.region_epoch == &b.mem.region_epoch);
        mem_free(&a.mem); mem_free(&b.mem);
    }
    {
        struct fixture a, b;
        setup(&a); setup(&b);
        const uint16_t words[] = {0xc000, 0xc000, 0x6c54, 0x2814, 0x3a54, 0x1801, 0, 0x3014};
        unsigned alias = 0x3a00;
        mem_set_ivram_area0(&b.mem, true);
        for (unsigned i = 0; i < sizeof words / sizeof *words; i++) {
            mem_write(&a.mem, ENTRY + 2 * i, 2, words[i]);
            mem_write(&b.mem, alias + 2 * i, 2, words[i]);
        }
        b.cpu.pc = alias;
        model.local_frontend = 1;
        for (unsigned i = 0; i < sizeof words / sizeof *words; i++) {
            c33_step(&a.cpu); c33_step(&b.cpu);
            assert(a.cpu.clk == b.cpu.clk);
            assert(!memcmp(a.cpu.r, b.cpu.r, sizeof a.cpu.r));
            assert(a.cpu.n_ext == b.cpu.n_ext && b.cpu.frontend_valid);
        }
        /* Removing the alias invalidates queued instruction words too. */
        mem_set_ivram_area0(&b.mem, false);
        c33_step(&b.cpu);
        assert(!b.cpu.frontend_valid && b.cpu.last_insn == 0);
        mem_free(&a.mem); mem_free(&b.mem);
    }
    {
        struct fixture a, b;
        setup(&a); setup(&b);
        model.local_frontend = 1;
        c33_step(&a.cpu);
        assert(a.cpu.frontend_valid);
        b.cpu = a.cpu;
        b.cpu.bus.ctx = &b.mem;
        b.cpu.region_epoch = &b.mem.region_epoch;
        b.cpu.fetch_lo = b.cpu.fetch_hi = b.cpu.data_lo = b.cpu.data_hi = 0;
        mem_write(&b.mem, b.cpu.pc, 2, 0x6c15);
        c33_step(&b.cpu);
        assert(b.cpu.frontend_valid && b.cpu.frontend.ctx == &b.cpu && b.cpu.r[5] == 1);
        mem_free(&a.mem); mem_free(&b.mem);
    }
    clock_progress();
    model = saved;
    printf("C33 experimental frontend: %u paired architectural state/RAM checks passed\n", checks);
    return 0;
}
