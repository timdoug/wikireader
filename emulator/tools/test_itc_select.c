/* Compare active-bit arbitration with the original per-vector rules. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/itc.h"

static const unsigned vectors[] = {
    19,20,22,23,24,25,30,31,34,35,38,39,42,43,46,47,50,51,
    56,57,58,60,61,62
};

static bool reference(const struct itc *t, unsigned *vector, unsigned *priority)
{
    unsigned best = 0;
    for (unsigned i = 0; i < sizeof vectors / sizeof *vectors; i++) {
        unsigned v = vectors[i], reg, bit;
        if (v == 19 || v == 20) {
            reg = 0x80; bit = v == 19 ? 3 : 4;
        } else if (v < 26) {
            reg = 0x81; bit = v - 22;
        } else if (v < 52) {
            unsigned channel = (v - 30) / 4;
            reg = 0x82 + channel / 2;
            bit = 2 + 4 * (channel & 1) + ((v - 30) & 1);
        } else {
            reg = 0x86; bit = v < 59 ? v - 56 : v - 57;
        }
        if (!(t->reg[reg] & (1u << bit)) || !itc_enabled(t, v))
            continue;
        unsigned p = itc_priority(t, v);
        if (p > best) {
            best = p; *vector = v; *priority = p;
        }
    }
    return best != 0;
}

static unsigned checks;
static void compare(struct itc *t)
{
    unsigned av = 999, ap = 999, bv = 999, bp = 999;
    bool a = itc_next_irq(t, &av, &ap), b = reference(t, &bv, &bp);
    if (a != b || av != bv || ap != bp) {
        fprintf(stderr, "selection %u: got %d/%u/%u, expected %d/%u/%u\n",
                checks, a, av, ap, b, bv, bp);
        exit(1);
    }
    checks++;
}

static uint32_t rng = 0x1633abcd;
static uint32_t random32(void)
{
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return rng;
}

int main(void)
{
    struct mem mem;
    struct itc t;
    if (!mem_init(&mem)) return 1;
    itc_attach(&mem, &t);
    /* All flag/enable combinations within each group, including unused bits. */
    const unsigned groups[] = {0x80,0x81,0x82,0x83,0x84,0x86};
    for (unsigned g = 0; g < 6; g++) {
        itc_reset(&t);
        memset(t.reg + 0x61, 0x77, 10);
        for (unsigned f = 0; f < 256; f++) {
            for (unsigned e = 0; e < 256; e++) {
                t.reg[groups[g]] = f;
                t.reg[groups[g] - 16] = e;
                compare(&t);
            }
        }
    }
    /* Mixed priorities, ties, zero priorities, direct DMA-style changes. */
    for (unsigned n = 0; n < 262144; n++) {
        for (unsigned i = 0x61; i < 0x90; i++) t.reg[i] = random32();
        compare(&t); compare(&t);
        itc_set_flag(&t, vectors[random32() % 24]);
        compare(&t); compare(&t);
        unsigned reg = 0x61 + random32() % 47;
        unsigned size = 1u << (random32() % 3);
        mem_write(&mem, REG_BASE + ITC_BASE + reg, size, random32());
        compare(&t);
        if (!(n & 255)) { itc_reset(&t); compare(&t); }
    }
    mem_free(&mem);
    printf("ITC selection: %u differential checks passed\n", checks);
    return 0;
}
