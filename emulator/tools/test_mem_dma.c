/* HSDMA software-trigger semantics against manual II.1.3/II.1.6.
 * No SD image, production firmware or fitted SPI timing is involved. */
#include <assert.h>
#include <stdio.h>
#include "../src/cmu.h"
#include "../src/dma.h"
#include "../src/itc.h"
#include "../src/model.h"
#include "../src/port.h"
#include "../src/sdcard.h"

#define R(a) (REG_BASE + (a))
static void setup(struct mem *m, unsigned size, unsigned mode, unsigned sm,
                  unsigned dm, unsigned count, uint32_t src, uint32_t dst)
{
    mem_write(m, R(0x112c), 2, 0);
    mem_write(m, R(0x119c), 2, 1);
    mem_write(m, R(0x119e), 2, 0);
    mem_write(m, R(0x298), 1, 0);
    mem_write(m, R(0x281), 1, 1);
    mem_write(m, R(0x112e), 2, 1);
    mem_write(m, R(0x1120), 2, count & 65535);
    mem_write(m, R(0x1122), 2, 0x8000 | (count >> 16));
    mem_write(m, R(0x1126), 2, (sm << 12) | (size == 2 ? 0x4000 : 0));
    mem_write(m, R(0x112a), 2, (dm << 12) | (mode << 14));
    mem_write(m, R(0x1162), 2, size == 4);
    mem_write(m, R(0x1164), 4, src);
    mem_write(m, R(0x1168), 4, dst);
}
static void start(struct mem *m)
{
    mem_write(m, R(0x112c), 2, 1);
    mem_write(m, R(0x29a), 1, 1);
    assert(mem_read(m, R(0x29a), 1) == 0);
}
int main(void)
{
    struct mem m;
    struct itc itc;
    struct cmu cmu;
    struct port port;
    struct sdcard sd;
    struct dma dma;
    uint64_t clock = 0;
    uint32_t src = DSTRAM_BASE, dst = IVRAM_BASE;
    assert(mem_init(&m));
    itc_attach(&m, &itc);
    cmu_attach(&m, &cmu);
    port_attach(&m, &port, &itc);
    assert(sd_attach(&m, &sd, NULL, &port, NULL, true));
    dma_attach(&m, &dma, &itc, &cmu, &sd);
    dma_set_clock(&dma, &clock);
    model.dma_extra = 30;
    model.dma_mem_extra = 0;
    for (unsigned i = 0; i < 1024; i++)
        mem_write(&m, src + i, 1, (i * 73) ^ (i >> 3));
    for (unsigned size = 1; size <= 4; size *= 2) {
        setup(&m, size, 1, 3, 3, 64 / size, src, dst);
        uint64_t before = clock;
        start(&m);
        assert(clock - before == 2 * 64 / size);
        assert(!(mem_read(&m, R(0x112c), 2) & 1));
        assert(mem_read(&m, R(0x281), 1) & 1);
        assert(mem_read(&m, R(0x1164), 4) == src + 64);
        assert(mem_read(&m, R(0x1168), 4) == dst + 64);
        for (unsigned i = 0; i < 64; i++)
            assert(mem_read(&m, dst+i, 1) == mem_read(&m, src+i, 1));
    }
    /* Pending trigger survives disabled channel; W1C cancels it. */
    setup(&m, 4, 0, 3, 3, 2, src, dst);
    mem_write(&m, R(0x29a), 1, 1);
    assert(mem_read(&m, R(0x112e), 2) == 1);
    mem_write(&m, R(0x112e), 2, 1);
    mem_write(&m, R(0x112c), 2, 1);
    assert(mem_read(&m, R(0x1120), 2) == 2);
    mem_write(&m, R(0x29a), 1, 1);
    assert(mem_read(&m, R(0x1120), 2) == 1);
    assert(!(mem_read(&m, R(0x281), 1) & 1));
    mem_write(&m, R(0x29a), 1, 1);
    assert(mem_read(&m, R(0x281), 1) & 1);
    setup(&m, 4, 1, 3, 3, 8, src, dst);
    mem_write(&m, R(0x29a), 1, 1);
    mem_write(&m, R(0x112c), 2, 1);
    assert(mem_read(&m, R(0x1168), 4) == dst + 32);
    /* Two separately triggered blocks repeat the source pattern. */
    setup(&m, 4, 2, 2, 3, (2 << 8) | 4, src, dst);
    start(&m);
    assert(mem_read(&m, R(0x1120), 2) == ((1 << 8) | 4));
    assert(mem_read(&m, R(0x1164), 4) == src);
    assert(mem_read(&m, R(0x1168), 4) == dst + 16);
    assert(!(mem_read(&m, R(0x281), 1) & 1));
    mem_write(&m, R(0x29a), 1, 1);
    assert(mem_read(&m, R(0x1120), 2) == 4);
    assert(!(mem_read(&m, R(0x112c), 2) & 1));
    for (unsigned i = 0; i < 32; i++)
        assert(mem_read(&m, dst+i, 1) == mem_read(&m, src+i%16, 1));
    /* Zero block length encodes 256 units, not an empty transfer. */
    setup(&m, 1, 2, 3, 3, 1 << 8, src, dst);
    start(&m);
    assert(mem_read(&m, R(0x1168), 4) == dst + 256);
    /* Backward transfer with address restoration in advanced mode. */
    setup(&m, 4, 1, 3, 3, 16, src + 60, dst + 60);
    mem_write(&m, R(0x1162), 2, 0x31);
    start(&m);
    assert(mem_read(&m, R(0x1164), 4) == src + 60);
    assert(mem_read(&m, R(0x1168), 4) == dst + 60);
    for (unsigned i = 0; i < 64; i++)
        assert(mem_read(&m, dst+i, 1) == mem_read(&m, src+i, 1));
    /* Fixed-source fill and high transfer-count bits. */
    setup(&m, 4, 1, 0, 3, 65537, src, SDRAM_BASE);
    start(&m);
    assert(mem_read(&m, R(0x1168), 4) == SDRAM_BASE + 65537 * 4);
    assert(mem_read(&m, SDRAM_BASE + 65536 * 4, 4) == mem_read(&m, src, 4));
    /* A software trigger cannot clock a channel selecting SPI. */
    setup(&m, 4, 1, 3, 3, 4, src, dst);
    mem_write(&m, R(0x298), 1, 9);
    start(&m);
    assert(mem_read(&m, R(0x1120), 2) == 4);
    /* Reject inaccessible A0 and misaligned word operands. */
    for (unsigned bad = 0; bad < 2; bad++) {
        setup(&m, 4, 1, 3, 3, 4, bad ? src + 1 : 0xc00, dst);
        uint64_t before = clock;
        start(&m);
        assert(clock == before);
        assert(mem_read(&m, R(0x1120), 2) == 4);
    }
    /* ITC reset preserves the attached trigger wiring. */
    itc_reset(&itc);
    setup(&m, 4, 1, 3, 3, 1, src, dst);
    start(&m);
    assert(mem_read(&m, R(0x281), 1) & 1);
    puts("memory DMA: widths, bus cycles, software/pending triggers, counters, block reset, backward/fixed addresses and inaccessible RAM pass");
    return 0;
}
