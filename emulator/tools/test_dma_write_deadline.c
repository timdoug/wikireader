/* A real SPI-triggered HSDMA/IDMA write remains pending through destination
 * waits. Check RAM and completion flags, including the SDRAMC write buffer.
 */
#include <assert.h>
#include <stdio.h>
#include "../src/c33.h"
#include "../src/cmu.h"
#include "../src/dma.h"
#include "../src/itc.h"
#include "../src/model.h"
#include "../src/port.h"
#include "../src/sdcard.h"
#include "../src/sdramc.h"

#define REG(a) (REG_BASE + (a))
#define SOURCE (IVRAM_BASE + 0x40u)
#define DEST (IVRAM_BASE + 0x100u)
struct waits {
    unsigned delay, calls;
    uint32_t dest;
    uint64_t end;
    mem_wait_fn backend;
    void *backend_ctx;
};
static unsigned failures, checks;
struct events { struct sdcard *sd; struct dma *dma; };

static void settle(void *ctx)
{
    struct events *events = ctx;
    sd_poll(events->sd);
    dma_poll(events->dma);
}

static uint64_t wait_write(void *ctx, enum mem_access kind, uint32_t addr,
                           unsigned size, uint64_t now)
{
    struct waits *w = ctx;
    uint64_t wait = w->backend ? w->backend(w->backend_ctx, kind, addr, size, now) : 0;
    if (kind != MEM_DMA_WRITE || addr != w->dest) return wait;
    w->calls++;
    wait += w->delay;
    w->end = now + wait;
    return wait;
}

static void check(const char *label, uint64_t got, uint64_t want)
{
    checks++;
    if (got != want) {
        printf("FAIL %s: got %llu want %llu\n", label,
               (unsigned long long)got, (unsigned long long)want);
        failures++;
    }
}

static void finish_descriptor_writes(struct dma *dma)
{
    /* Payload acceptance precedes IDMA's descriptor writeback. Let those
     * real phases finish before checking that the controller is idle. */
    for (unsigned i = 0; i < 32 && dma->qn; i++) {
        uint64_t next = dma->q[0].due;
        for (unsigned j = 1; j < dma->qn; j++)
            if (dma->q[j].due < next) next = dma->q[j].due;
        if (*dma->clock < next) *dma->clock = next;
        dma_poll(dma);
    }
}

static void run(unsigned delay, unsigned size, bool idma, bool external, bool late, unsigned cpuop,
                bool coherence)
{
    struct mem mem;
    struct cmu cmu;
    struct itc itc;
    struct port port;
    struct sdcard sd;
    struct dma dma;
    struct sdramc sdram;
    uint64_t clock = 0;
    uint32_t dest = external ? SDRAM_BASE + 0x1000 : DEST;
    struct waits waits = {.delay = delay, .dest = dest};
    unsigned sizecode = size == 4 ? 2 : size == 2 ? 1 : 0;
    uint32_t mask = size == 4 ? 0xffffffffu : (1u << (8 * size)) - 1;
    assert(mem_init(&mem));
    itc_attach(&mem, &itc);
    cmu_attach(&mem, &cmu);
    port_attach(&mem, &port, &itc);
    assert(sd_attach(&mem, &sd, NULL, &port, NULL, true));
    dma_attach(&mem, &dma, &itc, &cmu, &sd);
    sd_set_clock(&sd, &clock);
    dma_set_clock(&dma, &clock);
    if (external) {
        sdramc_attach(&mem, &sdram);
        mem_write(&mem, REG(0x1604), 4, 0x1243);
        mem_write(&mem, REG(0x1608), 4, 0xfff);
        mem_write(&mem, REG(0x1610), 4, 0x8000000b);
        mem_write(&mem, REG(0x1600), 4, 0x14);
        waits.backend = mem.wait;
        waits.backend_ctx = mem.wait_ctx;
    }
    mem_set_timing(&mem, wait_write, &waits);
    mem_write(&mem, dest, 4, 0xcccccccc);
    if (coherence) {
        /* Complete the instruction and data fills well before the SPI
         * trigger. Their contents remain usable until a write lands. */
        mem_wait(&mem, MEM_CPU_FETCH, dest, 2, 0);
        mem_wait(&mem, MEM_CPU_FETCH, dest + 14, 2, 100);
        mem_wait(&mem, MEM_CPU_READ, dest, 4, 200);
        clock = 1000;
        check("primed instruction line is a cache hit",
              mem_wait(&mem, MEM_CPU_FETCH, dest, 2, clock), 0);
    }
    uint64_t due;
    uint32_t value;
    if (idma) {
        mem_write(&mem, SOURCE, 4, 0x12345678);
        mem_write(&mem, REG(0x1100), 4, DSTRAM_BASE);
        mem_write(&mem, DSTRAM_BASE + 0x240, 4, sizecode << 16);
        mem_write(&mem, DSTRAM_BASE + 0x244, 4, 1);
        mem_write(&mem, DSTRAM_BASE + 0x248, 4, SOURCE);
        mem_write(&mem, DSTRAM_BASE + 0x24c, 4, dest);
        mem_write(&mem, REG(0x1105), 1, 1);
        mem_write(&mem, REG(0x1104), 1, 0xa4);
        due = clock + 38;
        value = 0x12345678 & mask;
    } else {
        port.reg[OFF_P3D] |= 1u << 3;
        port.reg[OFF_P5D] |= 1u << CS_SDCARD_BIT;
        mem_write(&mem, REG(0x1708), 4, ((size * 8 - 1) << 10) | 15u);
        mem_write(&mem, REG(0x1710), 4, 0);
        mem_write(&mem, REG(0x119c), 2, 1);
        mem_write(&mem, REG(0x1150), 2, cpuop & 8 ? 2 : 1);
        mem_write(&mem, REG(0x1152), 2, 0x8000);
        mem_write(&mem, REG(0x1156), 2, size == 2 ? 0x4000 : 0);
        mem_write(&mem, REG(0x115a), 2, cpuop & 8 ? 0 : 0x3000);
        mem_write(&mem, REG(0x1192), 2, size == 4 ? 1 : 0);
        mem_write(&mem, REG(0x1194), 4, REG(0x1700));
        mem_write(&mem, REG(0x1198), 4, dest);
        mem_write(&mem, REG(0x0299), 1, 0x90);
        mem_write(&mem, REG(0x115c), 2, 1);
        mem_write(&mem, REG(0x1704), 4, mask);
        clock = sd.deadline;
        sd_poll(&sd); dma_poll(&dma);
        due = clock + 2;
        value = mask;
    }
    assert(dma.qn == 1);
    printf("write wait=%u size=%u idma=%u sdram=%u late=%u\n", delay, size, idma, external, late);
    clock = due - 1;
    dma_poll(&dma);
    check("destination before write request", mem_read(&mem, dest, size), 0xcccccccc & mask);
    if (late && !external) {
        clock = due + delay + 20;
        dma_poll(&dma);
    } else {
        clock = due;
        dma_poll(&dma);
        /* Completion is the memory interface's acceptance boundary;
         * a posted SDRAM write may drain externally after that time. */
        uint64_t end = external ? waits.end + (idma ? 1 : 0) : due + delay;
        check("poll preserves the CPU clock", clock, due);
        if (end > due) {
            check("RAM unchanged while destination bus waits", mem_read(&mem, dest, size), 0xcccccccc & mask);
            check("completion flag waits for destination", itc.reg[0x81] & 8, 0);
            check("payload stays pending through destination wait", dma.qn, 1);
            if (coherence) {
                check("write reservation preserves completed instruction data",
                      mem_wait(&mem, MEM_CPU_FETCH, dest, 2, clock), 0);
                unsigned cached = 0;
                for (unsigned i = 0; i < SDRAMC_MAX_DQ; i++)
                    cached += sdram.dq[i].valid && sdram.dq[i].tag == dest;
                check("write reservation preserves completed data queue", cached, 1);
            }
            if (cpuop & 8) {
                mem_write(&mem, REG(0x1704), 4, mask);
                for (unsigned i = 0; i < 3 && sd.xfers < 2; i++) {
                    clock = sd.deadline;
                    assert(clock < end);
                    sd_poll(&sd); dma_poll(&dma);
                }
                check("second physical SPI character completed", sd.xfers, 2);
                check("next SPI trigger waits out the active DMA write", dma.hsdma_transfers, 1);
                check("blocked trigger remains pending", dma.reg[0x5e] & 1, 1);
                check("no completion flag while destination waits", itc.reg[0x81] & 8, 0);
                clock = end;
                dma_poll(&dma);
                check("next DMA read starts after destination acceptance", dma.hsdma_transfers, 2);
                check("only the later payload remains queued", dma.qn, 1);
                check("first accepted write does not complete the channel", itc.reg[0x81] & 8, 0);
                clock = end + 2 + delay - 1;
                dma_poll(&dma);
                check("second destination retains its own full wait", dma.qn, 1);
                clock = end + 2 + delay;
                dma_poll(&dma);
                check("both delayed writes complete", dma.qn, 0);
                check("last accepted write completes the channel", itc.reg[0x81] & 8, 8);
                check("each destination is timed exactly once", waits.calls, 2);
                mem_free(&mem);
                return;
            }
            if (cpuop) {
                struct c33 cpu = {0};
                struct events events = {&sd, &dma};
                cpu.bus = (struct c33_bus){.ctx = &mem, .read = mem_read, .write = mem_write,
                    .wait = mem_wait, .before_data = settle, .before_data_ctx = &events};
                if (cpuop & 4)
                    cpu.bus.region = (uint8_t *(*)(void *, uint32_t, uint32_t *, uint32_t *))mem_region;
                c33_reset(&cpu, 0x100);
                cpu.clk = due;
                cpu.r[1] = dest;
                cpu.r[0] = 0x87654321;
                mem_write(&mem, 0x100, 2, (cpuop & 3) == 1 ? 0x3010 : 0x3c10);
                sd_set_clock(&sd, &cpu.clk);
                dma_set_clock(&dma, &cpu.clk);
                c33_step(&cpu);
                if ((cpuop & 3) == 1)
                    check("CPU read after destination wait sees DMA payload", cpu.r[0], value);
                else
                    check("later CPU posted store follows completed DMA write", mem_read(&mem, dest, 4), 0x87654321);
                check("CPU wait settles the DMA completion flag", itc.reg[0x81] & 8, idma ? 0 : 8);
                if (idma) finish_descriptor_writes(&dma);
                check("CPU wait and descriptor writeback finish the DMA transaction", dma.qn, 0);
                mem_free(&mem);
                return;
            }
            clock = end - 1;
            dma_poll(&dma);
            check("RAM unchanged one clock before completion", mem_read(&mem, dest, size), 0xcccccccc & mask);
        }
        clock = end;
        dma_poll(&dma);
    }
    check("destination at actual completion", mem_read(&mem, dest, size), value);
    if (coherence) {
        unsigned cached = 0;
        for (unsigned i = 0; i < SDRAMC_MAX_DQ; i++)
            cached += sdram.dq[i].valid && sdram.dq[i].tag == dest;
        check("accepted DMA payload invalidates the data queue", cached, 0);
        check("accepted DMA payload makes instruction fetch miss",
              mem_wait(&mem, MEM_CPU_FETCH, dest, 2, clock) > 0, true);
    }
    if (idma) finish_descriptor_writes(&dma);
    check("payload and descriptor phases complete", dma.qn, 0);
    if (!idma) check("HSDMA completion flag at actual completion", itc.reg[0x81] & 8, 8);
    check("destination timing is charged once", waits.calls, 1);
    mem_free(&mem);
}

int main(void)
{
    model.dma_async = 1;
    model.dma_extra = model.dma_bus_hold = model.dma_reg_hold = 0;
    model.dma_cpu_penalty = 0;
    model.idma_extra = 38;
    for (unsigned size = 1; size <= 4; size *= 2)
    for (unsigned idma = 0; idma < 2; idma++) {
        run(0, size, idma, false, false, 0, false);
        run(1, size, idma, false, false, 0, false);
        run(37, size, idma, false, false, 0, false);
        run(37, size, idma, false, true, 0, false);
        run(0, size, idma, true, false, 0, false);
    }
    for (unsigned mapped = 0; mapped < 2; mapped++)
    for (unsigned idma = 0; idma < 2; idma++) {
        run(0, 4, idma, true, false, 1 + 4 * mapped, false);
        run(0, 4, idma, true, false, 2 + 4 * mapped, false);
    }
    run(100, 1, false, false, false, 8, false);
    model.iq_lookahead = 0;
    for (unsigned policy = 0; policy <= 2; policy++)
    for (unsigned size = 1; size <= 4; size *= 2)
    for (unsigned idma = 0; idma < 2; idma++) {
        model.iq_deferred = policy;
        run(37, size, idma, true, false, 0, true);
    }
    printf("DMA destination deadline: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
