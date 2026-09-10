/* Bulk copies selected from the physical C33 memory benchmark.
 * Small/unaligned copies retain libc; A0 batching also handles overlap.
 * DMA is synchronous, on idle HSDMA0, with bounded bus ownership. */
#include <grifo.h>
#include <regs.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "zim_copy.h"

extern size_t zim_memory_bank_size(void);

ZIM_COPY_STATS zim_copy_stats;
ZIM_COPY_DIAG zim_copy_diag;

static void __attribute__((noinline,section(".fastcode.copy")))
batch_copy(unsigned char *d, const unsigned char *s, size_t groups, int backward)
{
    if (backward) {
        asm volatile (
            ".balign 16\n1:\n\t"
            "sub %1,32\n\tsub %0,32\n\t"
            "ld.w %%r0,[%1]+\n\tld.w %%r1,[%1]+\n\t"
            "ld.w %%r2,[%1]+\n\tld.w %%r3,[%1]+\n\t"
            "ld.w %%r4,[%1]+\n\tld.w %%r5,[%1]+\n\t"
            "ld.w %%r6,[%1]+\n\tld.w %%r7,[%1]+\n\t"
            "ld.w [%0]+,%%r0\n\tld.w [%0]+,%%r1\n\t"
            "ld.w [%0]+,%%r2\n\tld.w [%0]+,%%r3\n\t"
            "ld.w [%0]+,%%r4\n\tld.w [%0]+,%%r5\n\t"
            "ld.w [%0]+,%%r6\n\tld.w [%0]+,%%r7\n\t"
            "sub %1,32\n\tsub %0,32\n\t"
            "sub %2,1\n\tjrne 1b"
            : "+&r"(d), "+&r"(s), "+&r"(groups) :
            : "r0","r1","r2","r3","r4","r5","r6","r7","memory","cc");
    } else {
        asm volatile (
            ".balign 16\n1:\n\t"
            "ld.w %%r0,[%1]+\n\tld.w %%r1,[%1]+\n\t"
            "ld.w %%r2,[%1]+\n\tld.w %%r3,[%1]+\n\t"
            "ld.w %%r4,[%1]+\n\tld.w %%r5,[%1]+\n\t"
            "ld.w %%r6,[%1]+\n\tld.w %%r7,[%1]+\n\t"
            "ld.w [%0]+,%%r0\n\tld.w [%0]+,%%r1\n\t"
            "ld.w [%0]+,%%r2\n\tld.w [%0]+,%%r3\n\t"
            "ld.w [%0]+,%%r4\n\tld.w [%0]+,%%r5\n\t"
            "ld.w [%0]+,%%r6\n\tld.w [%0]+,%%r7\n\t"
            "sub %2,1\n\tjrne 1b"
            : "+&r"(d), "+&r"(s), "+&r"(groups) :
            : "r0","r1","r2","r3","r4","r5","r6","r7","memory","cc");
    }
}

/* Called with IRQs masked, before changing/clearing controller state. */
static void __attribute__((section(".copycode"), noinline))
snapshot(ZIM_COPY_STATE *p, const void *d, const void *s, size_t n,
         unsigned enabled, unsigned tf, unsigned fdma)
{
    if (p->attempt) return;
    p->attempt = zim_copy_diag.attempts;
    p->dst = (uintptr_t)d; p->src = (uintptr_t)s; p->bytes = n;
    p->enabled = enabled; p->tf0 = tf; p->fdma = fdma;
    p->edma = REG_INT_EDMA;
    p->select = REG_HSDMA_HTGR1 | ((unsigned)REG_HSDMA_HTGR2 << 8);
    p->count0 = REG_HS0_CNT; p->control0 = REG_HS0_CTRL;
    p->adv0 = REG_HS0_ADVMODE; p->mode = REG_HS_CNTLMODE;
    if (p->mode & 1) {
        p->source0 = ((unsigned long)REG_HS0_ADV_SADR_H << 16) | REG_HS0_ADV_SADR_L;
        p->dest0 = ((unsigned long)REG_HS0_ADV_DADR_H << 16) | REG_HS0_ADV_DADR_L;
    } else {
        p->source0 = ((unsigned long)(REG_HS0_SADR_H & 0xfff) << 16) | REG_HS0_SADR_L;
        p->dest0 = ((unsigned long)(REG_HS0_DADR_H & 0xfff) << 16) | REG_HS0_DADR_L;
    }
    p->acctime = REG_HS_ACCTIME; p->gate = REG_CMU_GATEDCLK1;
    p->idma = REG_IDMA_EN;
}

static unsigned __attribute__((section(".copycode"))) enabled_channels(void)
{
    return (REG_HS0_EN & 1) | ((REG_HS1_EN & 1) << 1) |
           ((REG_HS2_EN & 1) << 2) | ((REG_HS3_EN & 1) << 3);
}

/* Manual II-1-46 and III-2-42: FHDM0 is indeterminate after reset. An otherwise
 * unconfigured, disabled channel with no IRQ owner can have this bit set.
 * Only recognize that reset configuration; preserve a configured owner's
 * pending completion, as well as every other channel's flags. IRQs masked. */
static int __attribute__((section(".copycode"), noinline)) reset_configuration(void)
{
    volatile uint16_t *hs = (volatile uint16_t *)(REG_BASE + 0x1120);
    volatile uint16_t *adv = (volatile uint16_t *)(REG_BASE + 0x1160);
    unsigned i;
    if (!(REG_RST_RESET & RSTONLY) || (REG_INT_EDMA & 1) ||
        (REG_HSDMA_HTGR1 & 15)) return 0;
    for (i = 0; i < 6; ++i)
        if (hs[i] || adv[i]) return 0;
    return 1;
}

/* Called only for disjoint, word-aligned RAM operands. Preserve the idle
 * channel and shared controller settings; never run beside the SD engines.
 * A failed partial copy can safely be repeated by the CPU. */
static int __attribute__((section(".copycode"), noinline))
dma_copy(unsigned char *d, const unsigned char *s, size_t n)
{
    volatile uint16_t *hs = (volatile uint16_t *)(REG_BASE + 0x1120);
    volatile uint16_t *adv = (volatile uint16_t *)(REG_BASE + 0x1160);
    uint16_t saved[12], mode, acctime;
    uint8_t select, irq;
    unsigned i;
    int ok;
    ++zim_copy_diag.attempts;
    if (zim_copy_diag.failed) { ++zim_copy_diag.latched; return 0; }
    critical_t state = critcal_enter();
    unsigned enabled = enabled_channels(), tf = REG_HS0_TF, fdma = REG_INT_FDMA;
    if (!enabled && !(tf & 1) && (fdma & 1) && reset_configuration()) {
        snapshot(&zim_copy_diag.first_reset, d, s, n, enabled, tf, fdma);
        REG_INT_FDMA = 1; /* write-one-to-clear only HSDMA0's reset cause */
        ++zim_copy_diag.reset_irq;
        fdma = REG_INT_FDMA;
    }
    if (enabled || (tf & 1) || (fdma & 1)) {
        if (enabled) ++zim_copy_diag.busy;
        if (tf & 1) ++zim_copy_diag.trigger;
        if (fdma & 1) ++zim_copy_diag.irq;
        snapshot(&zim_copy_diag.first_guard, d, s, n, enabled, tf, fdma);
        critical_exit(state);
        return 0;
    }
    for (i = 0; i < 6; ++i) { saved[i] = hs[i]; saved[i+6] = adv[i]; }
    mode = REG_HS_CNTLMODE; acctime = REG_HS_ACCTIME;
    select = REG_HSDMA_HTGR1; irq = REG_INT_EDMA;
    REG_HS_CNTLMODE = 1; REG_HS_ACCTIME = 0;
    REG_HSDMA_HTGR1 &= 0xf0; REG_INT_EDMA &= ~1u;
    REG_HS0_ADVMODE = 1;
    REG_HS0_CNT = n / 4; REG_HS0_CTRL = 0x8000;
    REG_HS0_SADR_H = 0x3000; REG_HS0_DADR_H = 0x7000;
    REG_HS0_ADV_SADR_L = (uintptr_t)s;
    REG_HS0_ADV_SADR_H = (uintptr_t)s >> 16;
    REG_HS0_ADV_DADR_L = (uintptr_t)d;
    REG_HS0_ADV_DADR_H = (uintptr_t)d >> 16;
    unsigned long start = timer_get();
    asm volatile("" : : : "memory");
    REG_HS0_EN = 1; REG_HSDMA_HSOFTTGR = 1;
    while ((REG_HS0_EN & 1) && timer_get() - start < 600000) { }
    ok = !(REG_HS0_EN & 1) && !REG_HS0_CNT &&
         !(REG_HS0_CTRL & 255) && (REG_INT_FDMA & 1);
    asm volatile("" : : : "memory");
    if (!ok) snapshot(&zim_copy_diag.first_fault, d, s, n,
                      enabled_channels(), REG_HS0_TF, REG_INT_FDMA);
    REG_HS0_EN = 0; REG_HS0_TF = 1; REG_INT_FDMA = 1;
    for (i = 0; i < 6; ++i) { adv[i] = saved[i+6]; hs[i] = saved[i]; }
    REG_HS_CNTLMODE = mode; REG_HS_ACCTIME = acctime;
    REG_HSDMA_HTGR1 = select; REG_INT_EDMA = irq;
    critical_exit(state);
    if (!ok) { ++zim_copy_stats.dma_errors; zim_copy_diag.failed = 1; }
    return ok;
}

static void *__attribute__((section(".copycode"), noinline))
large_copy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    uintptr_t da = (uintptr_t)d, sa = (uintptr_t)s;
    int overlap = da > sa ? da - sa < n : sa - da < n;
    if (n >= 4096) {
        if (overlap) ++zim_copy_diag.overlap;
        else if (sa < 0x10000000 || sa >= 0x12000000 || n > 0x12000000-sa)
            ++zim_copy_diag.source;
    }
    if (!overlap && n >= 4096 && sa >= 0x10000000 && sa < 0x12000000 &&
        n <= 0x12000000 - sa) {
        size_t bank = zim_memory_bank_size();
        zim_copy_diag.bank = bank;
        int ivram = da >= 0x80000 && da < 0x83000 && n <= 0x83000 - da;
        int other = da >= 0x10000000 && da < 0x12000000 && n <= 0x12000000-da &&
            sa / bank == (sa+n-1) / bank && da / bank == (da+n-1) / bank &&
            sa / bank != da / bank;
        if (ivram || other) {
            ++zim_copy_diag.eligible;
            while (n >= 4096) {
                size_t chunk = n > 16384 ? 16384 : n & ~(size_t)3;
                if (!dma_copy(d, s, chunk)) break;
                zim_copy_stats.dma_bytes += chunk;
                ++zim_copy_stats.dma_calls;
                d += chunk; s += chunk; n -= chunk;
            }
        } else ++zim_copy_diag.layout;
    }
    if (n >= 32) {
        size_t bytes = n & ~(size_t)31;
        zim_copy_stats.cpu_bytes += bytes;
        if (overlap && da > sa) {
            /* Align the end before the backward word batches. */
            size_t tail = n - bytes;
            while (tail--) { --n; d[n] = s[n]; }
            batch_copy(d+n, s+n, bytes/32, 1);
            return dst;
        }
        batch_copy(d, s, bytes/32, 0);
        d += bytes; s += bytes; n -= bytes;
    }
    while (n--) *d++ = *s++;
    return dst;
}

/* Keep the common short-copy entry free of DMA's register-save frame. */
void *__attribute__((section(".copycode")))
zim_copy(void *dst, const void *src, size_t n)
{
    if (n >= 4096) {
        ++zim_copy_diag.large;
        if (((uintptr_t)dst | (uintptr_t)src) & 3) ++zim_copy_diag.unaligned;
        else if (dst == src) ++zim_copy_diag.identical;
    }
    if (n < 256 || (((uintptr_t)dst | (uintptr_t)src) & 3) || dst == src)
        return memmove(dst, src, n);
    return large_copy(dst, src, n);
}
