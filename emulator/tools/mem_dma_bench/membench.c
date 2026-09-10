/* One-shot C33 memory-copy benchmark. Kernel and SD driver are unchanged.
 * Timed regions have no card I/O. The LCD stays on its IVRAM framebuffer;
 * only its unused window area is borrowed. See README.md for model limits. */
#include <grifo.h>
#include <regs.h>
#include <stdio.h>
#include <string.h>

#define BANK (4UL * 1024 * 1024)
#define MAX_BYTES (512UL * 1024)
#define WINDOW ((unsigned char *)0x81a00)
#define WINDOW_SIZE 5632
#define FILL_WORD (*(volatile uint32_t *)0x84400)
#define REPEATS 3
#define TIMEOUT (60UL * 1000000)

enum method { LIBC, BATCH, DMA8, DMA16, DMA32, FILL_CPU, FILL_DMA, BACK_CPU,
              BACK_DMA, BLOCK_DMA, BATCH_FAST };
static const char *names[] = { "libc", "batch8", "dma8", "dma16", "dma32",
    "fill-cpu", "fill-dma", "back-cpu", "back-dma", "block-reset", "batch8-a0" };
static unsigned char saved_window[WINDOW_SIZE];
static unsigned char *allocation, *source, *same_bank, *other_bank;
static unsigned cases, failed;
static unsigned long log_position;
static uint32_t saved_fill;
static uint16_t saved_mode, saved_acctime;
static uint8_t saved_select, saved_irq;
static uint16_t saved_hs[8], saved_adv[6];
static uint32_t dma_stop[6];

/* Model runner stops here after durable results; physical hardware chains ZIM. */
void __attribute__((noinline)) membench_done(void) { asm volatile("nop"); }

static int log_line(const char *text)
{
    size_t size = strlen(text);
    int h = file_open("membench.log", FILE_OPEN_WRITE);
    int ok;
    if (h < 0) return 0;
    ok = file_lseek(h, log_position) == FILE_ERROR_OK &&
         file_write(h, (void *)text, size) == (ssize_t)size;
    if (file_close(h) != FILE_ERROR_OK) ok = 0;
    if (ok) log_position += size;
    return ok;
}

/* Eight loads before eight stores: avoid two SDRAM row changes per word. */
static inline void __attribute__((always_inline)) batch_loop(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    unsigned count = n / 32;
    if (count) {
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
            : "+&r"(d), "+&r"(s), "+&r"(count) :
            : "r0","r1","r2","r3","r4","r5","r6","r7","memory","cc");
    }
    for (n &= 31; n; --n) *d++ = *s++;
}

static void __attribute__((noinline)) batch_copy(void *dst, const void *src, size_t n)
{ batch_loop(dst, src, n); }
static void __attribute__((noinline,section(".fastcode"))) batch_fast(void *dst, const void *src, size_t n)
{ batch_loop(dst, src, n); }

/* Both SD channels must be idle before taking HSDMA0. Successive mode owns
 * the bus until completion, so keep every run <=512 KiB, with IRQs masked
 * and the watchdog serviced immediately beforehand. No HALT dependency. */
static int dma_copy(unsigned char *dst, const unsigned char *src, size_t n,
                    unsigned width, unsigned sm, unsigned dm, int block)
{
    unsigned long start;
    unsigned units = n / width;
    unsigned remaining = block ? units / 16 : 1;
    unsigned encoded = block ? (remaining << 8) | 16 : units;
    REG_HS0_EN = 0;
    REG_HS0_TF = 1;
    REG_INT_FDMA = 1;
    REG_HS0_ADVMODE = width == 4;
    REG_HS0_CNT = encoded;
    REG_HS0_CTRL = 0x8000 | (encoded >> 16);
    REG_HS0_SADR_H = (sm << 12) | (width == 2 ? 0x4000 : 0);
    REG_HS0_DADR_H = (dm << 12) | (block ? 0x8000 : 0x4000);
    REG_HS0_ADV_SADR_L = (uint32_t)src;
    REG_HS0_ADV_SADR_H = (uint32_t)src >> 16;
    REG_HS0_ADV_DADR_L = (uint32_t)dst;
    REG_HS0_ADV_DADR_H = (uint32_t)dst >> 16;
    start = timer_get();
    REG_HS0_EN = 1;
    do {
        REG_HSDMA_HSOFTTGR = 1;
        while (block ? (((REG_HS0_CTRL & 255UL) << 8) |
                        (REG_HS0_CNT >> 8)) == remaining : (REG_HS0_EN & 1)) {
            if (timer_get() - start > TIMEOUT) {
                REG_HS0_EN = 0;
                return 0;
            }
        }
    } while (--remaining);
    asm volatile("" : : : "memory");
    return !(REG_HS0_EN & 1) && (REG_INT_FDMA & 1) &&
           !(REG_HS0_CTRL & 255) && REG_HS0_CNT == (block ? 16 : 0);
}

static uint32_t pattern(unsigned i)
{
    uint32_t x = i * 2654435761UL;
    return x ^ (x >> 13) ^ 0x9da673c1UL;
}

static int run_case(const char *layout, enum method method, unsigned char *dst,
                    unsigned char *src, unsigned n)
{
    char line[384];
    unsigned long ticks[REPEATS], median, minimum, maximum;
    unsigned mismatch = 0, first = n, guards = 1, ok = 1;
    int fill = method == FILL_CPU || method == FILL_DMA;
    int backward = method == BACK_CPU || method == BACK_DMA;
    int block = method == BLOCK_DMA;
    unsigned width = method == DMA8 ? 1 : method == DMA16 ? 2 : 4;
    snprintf(line, sizeof(line), "BEGIN case=%u layout=%s method=%s bytes=%u src=%08lx dst=%08lx\n",
        cases, layout, names[method], n, (unsigned long)src, (unsigned long)dst);
    if (!log_line(line)) return 0;
    lcd_at_xy(0, 3);
    lcd_printf("Case %u: %s     ", cases, names[method]);
    for (unsigned repeat = 0; repeat < REPEATS; ++repeat) {
        /* Pattern, poisoning, verification and logging are outside timing. */
        memset(dst - 16, 0xa5, n + 32);
        for (unsigned i = 0; i < (fill ? 1 : n/4); ++i)
            ((uint32_t *)src)[i] = pattern(i);
        if (fill) FILL_WORD = 0x3c3c3c3c;
        if (backward) {
            /* dst=src+32, so this checks actual overlapping memmove. */
            for (unsigned i = 0; i < n/4; ++i) ((uint32_t *)src)[i] = pattern(i);
        }
        watchdog(WATCHDOG_KEY);
        critical_t irq = critcal_enter();
        unsigned long begin = timer_get();
        switch (method) {
        case LIBC: memcpy(dst, src, n); break;
        case BATCH: batch_copy(dst, src, n); break;
        case BATCH_FAST: batch_fast(dst, src, n); break;
        case FILL_CPU: memset(dst, 0x3c, n); break;
        case BACK_CPU: memmove(dst, src, n); break;
        case BACK_DMA: ok = dma_copy(dst+n-4, src+n-4, n, 4, 1, 1, 0); break;
        case FILL_DMA: ok = dma_copy(dst, (const unsigned char *)0x84400, n, 4, 0, 3, 0); break;
        case BLOCK_DMA: ok = dma_copy(dst, src, n, 4, 2, 3, 1); break;
        default: ok = dma_copy(dst, src, n, width, 3, 3, 0); break;
        }
        ticks[repeat] = timer_get() - begin;
        dma_stop[0] = REG_HS0_CNT;
        dma_stop[1] = REG_HS0_CTRL;
        dma_stop[2] = REG_HS0_EN;
        dma_stop[3] = REG_INT_FDMA;
        dma_stop[4] = ((uint32_t)REG_HS0_ADV_SADR_H << 16) | REG_HS0_ADV_SADR_L;
        dma_stop[5] = ((uint32_t)REG_HS0_ADV_DADR_H << 16) | REG_HS0_ADV_DADR_L;
        REG_HS0_EN = 0;
        REG_HS0_TF = 1;
        REG_INT_FDMA = 1;
        critical_exit(irq);
        watchdog(WATCHDOG_KEY);
        mismatch = 0; first = n;
        for (unsigned i = 0; i < n/4; ++i) {
            uint32_t expected = fill ? 0x3c3c3c3c : pattern(block ? i % 16 : i);
            if (((uint32_t *)dst)[i] != expected) { if (!mismatch) first = i*4; ++mismatch; }
        }
        for (unsigned i = 0; i < 16; ++i) {
            if (!backward && dst[-16+(int)i] != 0xa5) guards = 0;
            if (dst[n+i] != 0xa5) guards = 0;
        }
        /* A backward copy must also preserve the source prefix. */
        if (backward) for (unsigned i = 0; i < 8; ++i)
            if (((uint32_t *)src)[i] != pattern(i)) guards = 0;
        if (!ok || mismatch || !guards) break;
    }
    if (!ok || mismatch || !guards) {
        snprintf(line, sizeof(line), "FAIL case=%u dma_ok=%u mismatch=%u first=%u guards=%u count=%08lx ctrl=%08lx en=%08lx flags=%08lx src=%08lx dst=%08lx\n",
                 cases, ok, mismatch, first, guards, (unsigned long)dma_stop[0],
                 (unsigned long)dma_stop[1], (unsigned long)dma_stop[2],
                 (unsigned long)dma_stop[3], (unsigned long)dma_stop[4], (unsigned long)dma_stop[5]);
        log_line(line);
        ++failed;
        return 0;
    }
    minimum = maximum = ticks[0];
    for (unsigned i = 1; i < REPEATS; ++i) {
        if (ticks[i] < minimum) minimum = ticks[i];
        if (ticks[i] > maximum) maximum = ticks[i];
    }
    median = ticks[0] + ticks[1] + ticks[2] - minimum - maximum;
    snprintf(line, sizeof(line), "RESULT case=%u layout=%s method=%s bytes=%u ticks=%lu,%lu,%lu min=%lu median=%lu max=%lu verified=1\n",
        cases, layout, names[method], n, ticks[0], ticks[1], ticks[2], minimum, median, maximum);
    ++cases;
    return log_line(line);
}

static void restore(void)
{
    REG_HS0_EN = 0;
    REG_HS0_TF = 1;
    REG_INT_FDMA = 1;
    for (unsigned i = 0; i < 6; ++i)
        ((volatile uint16_t *)(REG_BASE+0x1160))[i] = saved_adv[i];
    for (unsigned i = 0; i < 6; ++i)
        ((volatile uint16_t *)(REG_BASE+0x1120))[i] = saved_hs[i];
    REG_HS_ACCTIME = saved_acctime;
    REG_HS_CNTLMODE = saved_mode;
    REG_HSDMA_HTGR1 = saved_select;
    REG_INT_EDMA = saved_irq;
    FILL_WORD = saved_fill;
    memcpy(WINDOW, saved_window, WINDOW_SIZE);
}

int grifo_main(int argc, char **argv)
{
    static const unsigned sizes[] = {64,256,1024,4096,16384,65536,262144,524288};
    static const unsigned ivsizes[] = {64,1024,4096,5120};
    unsigned long marker;
    char line[512];
    int h, ok = 1;
    (void)argc; (void)argv;
    if (file_size("membench.on", &marker) != FILE_ERROR_OK)
        chain("zim.app started-from-init");
    if (file_delete("membench.on") != FILE_ERROR_OK)
        chain("zim.app started-from-init");
    /* Restore normal startup before touching DMA. A watchdog/power failure
     * cannot leave an automatically repeating benchmark behind. */
    h = file_create("init.ini", FILE_OPEN_WRITE);
    if (h < 0) chain("zim.app started-from-init");
    const char normal[] = "zim.ico : zim.app started-from-init\n";
    ok = file_write(h, (void *)normal, sizeof(normal)-1) == sizeof(normal)-1;
    if (file_close(h) != FILE_ERROR_OK) ok = 0;
    if (!ok) chain("zim.app started-from-init");
    h = file_create("membench.log", FILE_OPEN_WRITE);
    if (h < 0) chain("zim.app started-from-init");
    if (file_close(h) != FILE_ERROR_OK) chain("zim.app started-from-init");
    snprintf(line, sizeof(line), "MEMBENCH v1 build=%s %s clock_hz=60000000 repeats=%u setup_included=1 acctime=unlimited\n",
             __DATE__, __TIME__, REPEATS);
    if (!log_line(line)) chain("zim.app started-from-init");
    if ((REG_HS0_EN | REG_HS1_EN | REG_HS2_EN | REG_HS3_EN) & 1) {
        log_line("ABORT: DMA channel already active\n");
        chain("zim.app started-from-init");
    }
    allocation = memory_allocate(3 * BANK, "memory benchmark");
    if (!allocation) {
        log_line("ABORT: buffer allocation failed\n");
        chain("zim.app started-from-init");
    }
    source = (unsigned char *)(((uintptr_t)allocation + BANK - 1) & ~(BANK-1));
    source += 1024;
    same_bank = source + 1024 * 1024;
    other_bank = source + BANK;
    saved_mode = REG_HS_CNTLMODE;
    saved_acctime = REG_HS_ACCTIME;
    saved_select = REG_HSDMA_HTGR1;
    saved_irq = REG_INT_EDMA;
    saved_fill = FILL_WORD;
    for (unsigned i = 0; i < 8; ++i)
        saved_hs[i] = ((volatile uint16_t *)(REG_BASE+0x1120))[i];
    for (unsigned i = 0; i < 6; ++i)
        saved_adv[i] = ((volatile uint16_t *)(REG_BASE+0x1160))[i];
    memcpy(saved_window, WINDOW, WINDOW_SIZE);
    lcd_window_disable();
    REG_HS_CNTLMODE = 1;
    REG_HS_ACCTIME = 0;
    REG_HSDMA_HTGR1 &= 0xf0;
    REG_INT_EDMA &= ~1u;
    snprintf(line, sizeof(line), "CONFIG src=%08lx same=%08lx other=%08lx ivram=%08lx sdram_ctl=%08lx refresh=%08lx app=%08lx dma_gate=%08lx\n",
        (unsigned long)source, (unsigned long)same_bank, (unsigned long)other_bank,
        (unsigned long)WINDOW, (unsigned long)REG_SDRAMC_CTL,
        (unsigned long)REG_SDRAMC_REF,
        (unsigned long)REG_SDRAMC_APP, (unsigned long)REG_CMU_GATEDCLK1);
    ok = log_line(line);
    if (ok) {
        critical_t irq = critcal_enter();
        unsigned long before = timer_get();
        unsigned long overhead = timer_get() - before;
        critical_exit(irq);
        snprintf(line, sizeof(line), "TIMING timer_pair_ticks=%lu previous_mode=%u previous_acctime=%u rst=%u\n",
                 overhead, saved_mode, saved_acctime, REG_RST_RESET);
        ok = log_line(line);
    }
    lcd_clear(LCD_WHITE); lcd_at_xy(0,0); lcd_print("Memory benchmark\nPlease wait...");
    /* Start with a tiny DMA copy, before trying full buffers. */
    if (ok) ok = run_case("same", DMA32, same_bank, source, 64);
    for (unsigned layout = 0; ok && layout < 2; ++layout)
        for (unsigned n = 0; ok && n < sizeof(sizes)/sizeof(*sizes); ++n)
            for (unsigned method = 0; ok && method < 4; ++method)
                ok = run_case(layout ? "other" : "same", method == 3 ? DMA32 : method == 2 ? BATCH_FAST : method,
                    layout ? other_bank : same_bank, source, sizes[n]);
    for (unsigned n = 0; ok && n < sizeof(ivsizes)/sizeof(*ivsizes); ++n)
        for (unsigned method = 0; ok && method < 4; ++method)
            ok = run_case("ivram", method == 3 ? DMA32 : method == 2 ? BATCH_FAST : method, WINDOW+16, source, ivsizes[n]);
    for (unsigned layout = 0; ok && layout < 2; ++layout)
        for (unsigned width = DMA8; ok && width <= DMA16; ++width)
            ok = run_case(layout ? "other" : "same", width,
                layout ? other_bank : same_bank, source, 4096);
    for (unsigned n = 3; ok && n < sizeof(sizes)/sizeof(*sizes); n += 2)
        for (unsigned method = FILL_CPU; ok && method <= FILL_DMA; ++method)
            ok = run_case("fill", method, same_bank, source, sizes[n]);
    for (unsigned method = BACK_CPU; ok && method <= BACK_DMA; ++method)
        ok = run_case("overlap", method, source+32, source, 4096);
    if (ok) ok = run_case("repeat", BLOCK_DMA, same_bank, source, 4096);
    restore();
    memory_free(allocation, "memory benchmark");
    snprintf(line, sizeof(line), "END MEMBENCH cases=%u failed=%u complete=%d state_restored=1\n", cases, failed, ok);
    log_line(line);
    lcd_clear(LCD_WHITE); lcd_at_xy(0,0);
    lcd_print(ok ? "Benchmark complete.\nStarting Wikipedia..." : "Benchmark stopped.\nResults saved.");
    membench_done();
    chain("zim.app started-from-init");
}
