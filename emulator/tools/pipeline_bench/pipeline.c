/* Prefix compression and load forwarding, with no DMA or timed I/O.
 * All templates use caller-saved registers and PC-relative local branches,
 * so the same bytes execute from SDRAM or the application's owned A0 RAM.
 */
#include <grifo.h>
#include <regs.h>
#include <stdio.h>
#include <string.h>

#define PASSES 20000UL
#define REPEATS 3
#define UNROLL 8
typedef uint32_t probe_fn(unsigned long, volatile uint32_t *, volatile uint32_t *);

#define PROBE(name, body) \
extern probe_fn name; extern char name##_end[]; \
asm(".pushsection .text.probes,\"ax\"\n.balign 16\n.global " #name "\n" #name ":\n" \
    "ld.w %r10,5\nld.w %r11,0\nld.w %r12,16\n.balign 16\n" \
    ".L" #name "_loop:\n.rept 8\n" body "\n.endr\n" \
    "sub %r6,1\njrne .L" #name "_loop\nld.w %r4,%r10\nret\n" \
    ".global " #name "_end\n" #name "_end:\n.popsection\n")

PROBE(base, "ld.w %r10,%r10");
PROBE(ext1, "ext 7\nand %r10,-2");
PROBE(ext2, "ext 0\next 7\nand %r10,-2");
PROBE(ldh, "ld.h %r10,[%r7]");
PROBE(ldw, "ld.w %r10,[%r7]");
PROBE(store, "ld.w [%r8],%r10");
PROBE(ext1_load, "ext 0\nld.w %r10,[%r7]");
PROBE(ext2_load, "ext 0\next 0\nld.w %r10,[%r7]");
PROBE(ext1_store, "ext 0\nld.w [%r8],%r10");
PROBE(ext2_store, "ext 0\next 0\nld.w [%r8],%r10");
PROBE(load_ext, "ld.h %r10,[%r7]\next 7\nand %r10,-2");
PROBE(move_ext, "ld.w %r10,%r10\next 7\nand %r10,-2");
PROBE(load_use0, "ld.h %r10,[%r7]\nxor %r11,%r10");
PROBE(load_use1, "ld.h %r10,[%r7]\nld.w %r12,%r12\nxor %r11,%r10");
PROBE(load_use2, "ld.h %r10,[%r7]\nld.w %r12,%r12\nld.w %r12,%r12\nxor %r11,%r10");
PROBE(word_use0, "ld.w %r10,[%r7]\nxor %r11,%r10");
PROBE(word_use1, "ld.w %r10,[%r7]\nld.w %r12,%r12\nxor %r11,%r10");
PROBE(ext1_branch, "cmp %r11,%r11\next 0\njreq 2f\n2:");
PROBE(ext2_branch, "cmp %r11,%r11\next 0\next 0\njreq 2f\n2:");
/* A short mix of the constant construction, loads and dependent ALU work
 * that the RISC-V interpreter exercises, rather than an ext-only stream. */
PROBE(mixed, "ext 0\next 0\nld.w %r10,5\nld.h %r10,[%r7]\n"
             "ext 7\nand %r10,-2\nxor %r11,%r10");

struct probe { const char *name; probe_fn *fn; char *end; unsigned result; };
#define ROW(name, result) { #name, name, name##_end, result }
static const struct probe probes[] = {
    ROW(base, 5), ROW(ext1, 4), ROW(ext2, 4),
    ROW(ldh, 5), ROW(ldw, 5), ROW(store, 5),
    ROW(ext1_load, 5), ROW(ext2_load, 5),
    ROW(ext1_store, 5), ROW(ext2_store, 5),
    ROW(load_ext, 4), ROW(move_ext, 4),
    ROW(load_use0, 5), ROW(load_use1, 5), ROW(load_use2, 5),
    ROW(word_use0, 5), ROW(word_use1, 5),
    ROW(ext1_branch, 5), ROW(ext2_branch, 5), ROW(mixed, 4),
};

/* Application-owned memory, allocated by Grifo's ELF loader. Neither the
 * kernel's scratch RAM nor the framebuffer/IVRAM window is borrowed. */
static unsigned char code[512] __attribute__((section(".fastbss"), aligned(16)));
static volatile uint32_t local[4] __attribute__((section(".fastbss"), aligned(16)));
static volatile uint32_t external[4] __attribute__((aligned(16)));
static unsigned long log_position;
static unsigned cases, failures;

static int log_line(const char *line)
{
    unsigned len = strlen(line);
    int h = file_open("pipeline.log", FILE_OPEN_WRITE);
    if (h < 0) return 0;
    int ok = file_lseek(h, log_position) == FILE_ERROR_OK &&
             file_write(h, (void *)line, len) == (ssize_t)len;
    if (file_close(h) != FILE_ERROR_OK) ok = 0;
    if (ok) log_position += len;
    debug_print(line);
    return ok;
}

int grifo_main(int argc, char **argv)
{
    int power = argc > 1 && strcmp(argv[1], "off") == 0;
    char line[320];
    int h = file_create("pipeline.log", FILE_OPEN_WRITE);
    if (h < 0 || file_close(h) != FILE_ERROR_OK) return 1;
    snprintf(line, sizeof line,
             "PIPELINE v1 passes=%lu unroll=%u repeats=%u "
             "sdram_ctl=%08lx refresh=%08lx clkcntl=%08lx pll=%08lx "
             "a0=%08lx sdram=%08lx code=%08lx\n",
             PASSES, UNROLL, REPEATS, (unsigned long)REG_SDRAMC_CTL,
             (unsigned long)REG_SDRAMC_REF, (unsigned long)REG_CMU_CLKCNTL,
             (unsigned long)REG_CMU_PLL, (unsigned long)local,
             (unsigned long)external, (unsigned long)code);
    if (!log_line(line)) return 1;
    lcd_clear(LCD_WHITE);
    lcd_at_xy(0, 0);
    lcd_printf("Pipeline timing probe\nResults: pipeline.log\n");
    for (unsigned i = 0; i < sizeof probes / sizeof *probes; i++) {
        const struct probe *p = &probes[i];
        unsigned size = p->end - (char *)p->fn;
        if (size + 14 > sizeof code) { failures++; break; }
        /* Original SDRAM code, then the identical bytes at every even
         * position within a sixteen-byte A0 fetch line. */
        for (int placement = -1; placement < 8; placement++) {
            unsigned offset = placement < 0 ? 0 : 2 * placement;
            probe_fn *fn = p->fn;
            if (placement >= 0) {
                memcpy(code + offset, (void *)p->fn, size);
                fn = (probe_fn *)(code + offset);
            }
            for (unsigned memory = 0; memory < 2; memory++) {
                volatile uint32_t *data = memory ? external : local;
                unsigned long ticks[REPEATS];
                unsigned good = 1;
                for (unsigned r = 0; r < REPEATS; r++) {
                    data[0] = 0x13579bdf; data[1] = 5;
                    data[2] = 5; data[3] = 0x2468ace0;
                    watchdog(WATCHDOG_KEY);
                    critical_t state = critcal_enter();
                    unsigned long start = timer_get();
                    uint32_t result = fn(PASSES, data + 1, data + 2);
                    ticks[r] = timer_get() - start;
                    critical_exit(state);
                    good &= result == p->result && data[0] == 0x13579bdf &&
                            data[1] == 5 && data[2] == 5 && data[3] == 0x2468ace0;
                }
                cases++;
                failures += !good;
                snprintf(line, sizeof line,
                         "RESULT probe=%s code=%s offset=%u data=%s bytes=%u "
                         "passes=%lu ticks=%lu,%lu,%lu result=%u ok=%u\n",
                         p->name, placement < 0 ? "sdram" : "a0", offset,
                         memory ? "sdram" : "a0", size, PASSES,
                         ticks[0], ticks[1], ticks[2], p->result, good);
                if (!log_line(line)) return 1;
                lcd_at_xy(0, 3);
                lcd_printf("%u cases, %u failed  ", cases, failures);
            }
        }
    }
    snprintf(line, sizeof line, "END PIPELINE cases=%u failures=%u\n", cases, failures);
    if (!log_line(line)) return 1;
    lcd_at_xy(0, 5);
    lcd_printf("Done. Tap to return.\n");
    if (power) power_off();
    event_t event;
    event_flush();
    do { event_wait(&event, NULL, NULL); }
    while (event.item_type != EVENT_TOUCH_DOWN && event.item_type != EVENT_BUTTON_DOWN);
    return failures != 0;
}
