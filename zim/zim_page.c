/* Optional retrieval timings. Wrappers leave existing hot loops in place.
 * Freeze measurements before rendering; write only after render returns. */
#include <grifo.h>
#include <stdio.h>
#include "zim_startup.h"
#include "zim_copy.h"

extern int __real_retrieve_article(long);
extern int __real_render_article_with_pcf(void);
#define LOG_CODE __attribute__((section(".copycode")))
static int pending, result;
static unsigned long article, elapsed;
static ZIM_COPY_STATS copies;
static ZIM_COPY_STATS total_copies;
static ZIM_COPY_DIAG diagnostic;
static file_io_stats_t io;

int LOG_CODE __wrap_retrieve_article(long index)
{
    if (!zim_startup_logging()) return __real_retrieve_article(index);
    unsigned long start;
    ZIM_COPY_STATS before = zim_copy_stats;
    file_profile(&io, true);
    start = timer_get();
    result = __real_retrieve_article(index);
    elapsed = timer_get() - start;
    file_profile(&io, false);
    copies.cpu_bytes = zim_copy_stats.cpu_bytes - before.cpu_bytes;
    copies.dma_bytes = zim_copy_stats.dma_bytes - before.dma_bytes;
    copies.dma_calls = zim_copy_stats.dma_calls - before.dma_calls;
    copies.dma_errors = zim_copy_stats.dma_errors - before.dma_errors;
    /* Freeze cumulative state too: a failure while drawing Search can
     * disable DMA before the first retrieval's delta counters start. */
    total_copies = zim_copy_stats;
    diagnostic = zim_copy_diag;
    article = (unsigned long)index;
    pending = 1;
    return result;
}

static int LOG_CODE log_state(int h, const char *kind, const ZIM_COPY_STATE *p)
{
    char line[512];
    if (!p->attempt) return 1;
    int n = snprintf(line, sizeof(line),
        "ZIMCOPY_STATE v1 kind=%s attempt=%lu dst=%08lx src=%08lx bytes=%lu "
        "enabled=%lx tf0=%lx fdma=%lx edma=%lx select=%04lx "
        "count0=%lx control0=%lx source0=%08lx dest0=%08lx adv0=%lx "
        "mode=%lx acctime=%lx gate=%08lx idma=%lx\n",
        kind, p->attempt, p->dst, p->src, p->bytes, p->enabled, p->tf0,
        p->fdma, p->edma, p->select, p->count0, p->control0,
        p->source0, p->dest0, p->adv0, p->mode, p->acctime, p->gate, p->idma);
    return n > 0 && n < (int)sizeof(line) && file_write(h, line, n) == n;
}

int LOG_CODE __wrap_render_article_with_pcf(void)
{
    int rendered = __real_render_article_with_pcf();
    if (pending) {
        char line[512];
        unsigned long size = 0;
        int h, n;
        pending = 0;
        n = snprintf(line, sizeof(line),
            "ZIMPAGE v1 build=%s %s index=%lu result=%d retrieve_us=%lu "
            "read_calls=%lu read_sectors=%lu read_us=%lu read_errors=%lu "
            "copy_cpu_bytes=%lu copy_dma_bytes=%lu copy_dma_calls=%lu copy_dma_errors=%lu\n",
            __DATE__, __TIME__, article, result, elapsed / TIMER_CountsPerMicroSecond,
            (unsigned long)io.read_calls, (unsigned long)io.read_sectors,
            (unsigned long)io.read_ticks / TIMER_CountsPerMicroSecond,
            (unsigned long)io.read_errors, copies.cpu_bytes, copies.dma_bytes,
            copies.dma_calls, copies.dma_errors);
        if (n <= 0 || n >= (int)sizeof(line)) return rendered;
        if (file_size("0:/zimpage.log", &size) == FILE_ERROR_OK && size < 65536) {
            h = file_open("0:/zimpage.log", FILE_OPEN_READ | FILE_OPEN_WRITE);
            if (h >= 0 && file_lseek(h, size) != FILE_ERROR_OK) { file_close(h); return rendered; }
        } else h = file_create("0:/zimpage.log", FILE_OPEN_WRITE);
        if (h >= 0) {
            if (file_write(h, line, n) == n) {
                /* Decimal counters are lifetime totals; register snapshots
                 * are hex. Diagnostics never clear a rejected DMA flag. */
                n = snprintf(line, sizeof(line),
                    "ZIMCOPY v1 scope=cumulative large=%lu unaligned=%lu identical=%lu "
                    "overlap=%lu source=%lu layout=%lu eligible=%lu bank=%lu "
                    "attempts=%lu latched=%lu busy=%lu trigger=%lu irq=%lu failed=%lu "
                    "dma_calls=%lu dma_errors=%lu reset_irq=%lu\n",
                    diagnostic.large, diagnostic.unaligned, diagnostic.identical,
                    diagnostic.overlap, diagnostic.source, diagnostic.layout,
                    diagnostic.eligible, diagnostic.bank, diagnostic.attempts,
                    diagnostic.latched, diagnostic.busy, diagnostic.trigger,
                    diagnostic.irq, diagnostic.failed,
                    total_copies.dma_calls, total_copies.dma_errors, diagnostic.reset_irq);
                if (n > 0 && n < (int)sizeof(line) && file_write(h, line, n) == n &&
                    log_state(h, "reset", &diagnostic.first_reset) &&
                    log_state(h, "guard", &diagnostic.first_guard))
                    log_state(h, "fault", &diagnostic.first_fault);
            }
            file_close(h);
        }
    }
    return rendered;
}
