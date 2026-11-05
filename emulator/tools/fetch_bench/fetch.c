/* Controlled external-fetch experiments. All timed entry/exit code lives
 * in owned A0 RAM; the actual templates are copied to controlled addresses.
 * Boot only through FLASH -> Grifo -> init, like the reader. */
#include <grifo.h>
#include <regs.h>
#include <stdio.h>
#include <string.h>

#define REPEATS 3
typedef unsigned probe_fn(unsigned, volatile unsigned char *, volatile unsigned char *);

#define NORMAL "sub %r6,1\ncmp %r6,0\njrne 1b\n"
#define DELAYED "sub %r6,1\njrne.d 1b\nnop\n"
/* Exactly 16 bytes of setup, then a position-independent measured loop.
 * Equal-sized branch endings retain the same body size and operation count.
 * A delayed branch uses only the documented one-cycle NOP delay slot. */
#define TEMPLATE(name, count, load, store, tail) \
extern probe_fn name; extern char name##_end[]; \
asm(".pushsection .text.probes,\"ax\"\n.balign 16\n.global " #name "\n" #name ":\n" \
    "ld.w %r10,5\nld.w %r11,0\nld.w %r12,1\nnop\nnop\nnop\nnop\nnop\n" \
    "1:\n" load "\n.rept " #count "\nadd %r11,%r12\n.endr\n" store "\n" tail \
    "ld.w %r4,%r10\nxor %r4,%r11\nret\n.global " #name "_end\n" #name "_end:\n.popsection\n")

#define PAIR(name, count, load, store) \
TEMPLATE(name##_n, count, load, store, NORMAL); \
TEMPLATE(name##_d, count, load, store, DELAYED)
PAIR(alu8, 8, "ld.w %r10,%r10", "ld.w %r10,%r10");
PAIR(hold8, 8, "ld.ub %r10,[%r7]", "ld.b [%r8],%r10");
PAIR(copy0, 0, "ld.ub %r10,[%r7]+", "ld.b [%r8]+,%r10");
PAIR(copy4, 4, "ld.ub %r10,[%r7]+", "ld.b [%r8]+,%r10");
PAIR(copy8, 8, "ld.ub %r10,[%r7]+", "ld.b [%r8]+,%r10");

struct probe {
    const char *name;
    probe_fn *fn[2];
    char *end[2];
    unsigned padding, kind;
};
#define ROW(name, padding, kind) { #name, {name##_n, name##_d}, {name##_n_end, name##_d_end}, padding, kind }
static const struct probe probes[] = {
    ROW(alu8, 8, 0), ROW(hold8, 8, 1), ROW(copy0, 0, 2),
    ROW(copy4, 4, 2), ROW(copy8, 8, 2),
};

/* Keep code in .data, before the separately aligned external BSS bank.
 * Never straddle a 1-KiB row: all tested loops start at code+272+offset. */
static unsigned char sdram_code[1024] __attribute__((aligned(1024))) = {1};
static volatile unsigned char external[0x40000] __attribute__((aligned(0x400000)));
static unsigned char local_code[128] __attribute__((section(".fastbss"), aligned(16)));
static volatile unsigned char local_data[16] __attribute__((section(".fastbss"), aligned(16)));
static unsigned char local_stack[128] __attribute__((section(".fastbss"), aligned(16)));

struct measurement {
    probe_fn *fn;
    unsigned passes, iqb;
    volatile unsigned char *src, *dst;
    unsigned result;
};
static struct measurement work __attribute__((section(".fastbss"), aligned(16)));

static __attribute__((always_inline)) inline unsigned clock_read(void)
{
    unsigned high1, low, high2;
    do {
        high1 = REG_T16_TC5;
        low = REG_T16_TC0;
        high2 = REG_T16_TC5;
    } while (high1 != high2);
    return (high2 << 16) | low;
}

static unsigned __attribute__((section(".fastcode"), noinline)) measure(struct measurement *m)
{
    unsigned saved = REG_SDRAMC_APP;
    probe_fn *fn = m->fn;
    unsigned passes = m->passes;
    volatile unsigned char *src = m->src, *dst = m->dst;
    unsigned saved_sp;
    /* Calls push return addresses. Keep that traffic in owned A0 RAM so
     * the measuring call/return does not introduce an external data miss.
     * Build validation checks that the register-only wrapper restores SP
     * before its epilogue; no C stack objects live across this switch. */
    asm volatile("ld.w %0,%%sp" : "=r"(saved_sp));
    asm volatile("ld.w %%sp,%0" :: "r"(local_stack + sizeof local_stack) : "memory");
    /* A0 code makes both configuration writes and the timer boundary
     * independent of external instruction traffic. Interrupts are already
     * disabled by the caller. These writes are outside the timed interval. */
    REG_SDRAMC_APP = saved & ~1u;
    REG_SDRAMC_APP = (saved & ~1u) | m->iqb;
    /* The manual guarantees invalidation on a matching data write, but
     * does not promise that an IQBEN toggle empties the buffers. Rewrite
     * one unchanged halfword in every copied-code line, then read back
     * the final location, all before timing. This defines the preparation
     * traffic and open-row context without assuming toggle semantics. */
    volatile unsigned short *line = (volatile unsigned short *)((unsigned long)fn & ~15UL);
    for (unsigned i = 0; i < 8; i++) line[i * 8] = line[i * 8];
    (void)line[56];
    unsigned start = clock_read();
    unsigned result = fn(passes, src, dst);
    unsigned end = clock_read();
    m->result = result;
    REG_SDRAMC_APP = saved;
    asm volatile("ld.w %%sp,%0" :: "r"(saved_sp) : "memory");
    return end - start;
}

static unsigned log_position, cases, failures;
static int log_line(char *line)
{
    unsigned length = strlen(line);
    int h = file_open("fetchq.log", FILE_OPEN_WRITE);
    if (h < 0) return 0;
    int ok = file_lseek(h, log_position) == FILE_ERROR_OK &&
             file_write(h, line, length) == (ssize_t)length;
    if (file_close(h) != FILE_ERROR_OK) ok = 0;
    if (ok) log_position += length;
    debug_print(line);
    return ok;
}

static int run_case(const struct probe *p, unsigned branch, unsigned code_local,
                    unsigned offset, unsigned data_external, unsigned iqb, unsigned passes)
{
    unsigned bytes = p->end[branch] - (char *)p->fn[branch];
    unsigned char *buffer = code_local ? local_code : sdram_code + 256;
    volatile unsigned char *src = data_external ? external + 0x1000 : local_data + 4;
    volatile unsigned char *dst = data_external ? external + 0x21000 : local_data + 8;
    unsigned used = p->kind == 2 ? passes : 1;
    unsigned ticks[REPEATS], result = 0, ok = 1;
    char line[384];
    if (bytes + offset > 128 || used > 8192 || (p->kind == 2 && !data_external)) return 0;
    memset(buffer, 0, 128);
    memcpy(buffer + offset, (void *)p->fn[branch], bytes);
    work.fn = (probe_fn *)(buffer + offset);
    work.src = src; work.dst = dst; work.passes = passes; work.iqb = iqb;
    unsigned expected = (p->kind ? 13 : 5) ^ (p->padding * passes);
    for (unsigned r = 0; r < REPEATS; r++) {
        memset(local_stack, 0x6b, 16);
        for (unsigned i = 0; i < used; i++) { src[i] = 13; dst[i] = 0xa5; }
        src[-1] = 0x39; src[used] = 0x93;
        dst[-1] = 0x47; dst[used] = 0x74;
        watchdog(WATCHDOG_KEY);
        critical_t state = critcal_enter();
        ticks[r] = measure(&work);
        critical_exit(state);
        result = work.result;
        ok &= result == expected && ticks[r] > 0 &&
              src[-1] == 0x39 && src[used] == 0x93 && dst[-1] == 0x47 && dst[used] == 0x74;
        for (unsigned i = 0; i < used; i++)
            ok &= src[i] == 13 && dst[i] == (p->kind ? 13 : 0xa5);
        for (unsigned i = 0; i < 16; i++) ok &= local_stack[i] == 0x6b;
    }
    cases++; failures += !ok;
    snprintf(line, sizeof line,
             "RESULT probe=%s branch=%s code=%s offset=%u data=%s iqb=%u passes=%u "
             "bytes=%u entry=%08lx loop=%08lx src=%08lx dst=%08lx "
             "ticks=%u,%u,%u result=%u ok=%u\n", p->name, branch ? "delayed" : "normal",
             code_local ? "a0" : "sdram", offset, data_external ? "sdram" : "a0", iqb, passes,
             bytes, (unsigned long)work.fn, (unsigned long)work.fn + 16,
             (unsigned long)src, (unsigned long)dst, ticks[0], ticks[1], ticks[2], result, ok);
    return log_line(line);
}

int grifo_main(int argc, char **argv)
{
    static const unsigned passes[] = {1, 8, 8192};
    char line[384];
    int power = argc > 1 && strcmp(argv[1], "off") == 0;
    int h = file_create("fetchq.log", FILE_OPEN_WRITE);
    if (h < 0 || file_close(h) != FILE_ERROR_OK) return 1;
    if (((unsigned long)sdram_code >> 22) == ((unsigned long)external >> 22)) return 1;
    snprintf(line, sizeof line,
             "FETCHQ v1 repeats=%u sdram_ctl=%08lx refresh=%08lx app=%08lx "
             "clkcntl=%08lx pll=%08lx timer0=%04x clock0=%04x timer5=%04x clock5=%04x "
             "code=%08lx data=%08lx a0=%08lx wrapper=%08lx stack=%08lx\n", REPEATS,
             (unsigned long)REG_SDRAMC_CTL, (unsigned long)REG_SDRAMC_REF,
             (unsigned long)REG_SDRAMC_APP, (unsigned long)REG_CMU_CLKCNTL,
             (unsigned long)REG_CMU_PLL, REG_T16_CTL0, REG_T16_CLKCTL_0,
             REG_T16_CTL5, REG_T16_CLKCTL_5, (unsigned long)sdram_code,
             (unsigned long)external, (unsigned long)local_code, (unsigned long)measure,
             (unsigned long)local_stack);
    if (!log_line(line)) return 1;
    lcd_clear(LCD_WHITE); lcd_at_xy(0, 0); lcd_printf("External fetch probe\nResults: fetchq.log\n");
    for (unsigned p = 0; p < sizeof probes / sizeof *probes; p++)
    for (unsigned branch = 0; branch < 2; branch++)
    for (unsigned data = probes[p].kind == 2; data < (probes[p].kind ? 2u : 1u); data++)
    for (unsigned offset = 0; offset < 16; offset += 2)
    for (unsigned iqb = 0; iqb < 2; iqb++)
    for (unsigned n = 0; n < 3; n++) {
        if (!run_case(&probes[p], branch, 0, offset, data, iqb, passes[n])) return 1;
        lcd_at_xy(0, 3); lcd_printf("%u cases, %u failed  ", cases, failures);
    }
    /* Independent A0 controls: IQBEN should affect external instruction
     * fetching, not introduce a generic per-instruction CPU penalty. */
    for (unsigned p = 0; p < sizeof probes / sizeof *probes; p++) {
        if (p != 0 && p != 1 && p != 3) continue;
        for (unsigned branch = 0; branch < 2; branch++)
        for (unsigned iqb = 0; iqb < 2; iqb++)
        for (unsigned n = 1; n < 3; n++)
            if (!run_case(&probes[p], branch, 1, 0, probes[p].kind == 2, iqb, passes[n])) return 1;
    }
    snprintf(line, sizeof line, "END FETCHQ cases=%u failures=%u\n", cases, failures);
    if (!log_line(line)) return 1;
    lcd_at_xy(0, 5); lcd_printf("Done.\n");
    if (power) power_off();
    return failures != 0;
}
