/* Persistent, bounded performance logs, GPL-3.0-or-later. */
#include <grifo.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "profile.h"
#ifdef WR_C33
#include <regs.h>
#include "build/profile_id.h"
#else
#define WR_BUILD_ID "host-test"
#endif

#define HZ 60000000U
#define LOG_PATH "0:/doomperf.log"
#define LOG_LIMIT (1024U * 1024U)
#define U(v) ((unsigned long)(v))
#define US(v) U((uint32_t)(v) / 60U)

int wr_profile_enabled;
static unsigned bench; /* 0: play, 1: wait for level, 2: warmup, 3: sample, 4: play */
static int continuous;
static uint32_t app_start, last_end, window_start, frame_start, engine_end;
static uint32_t phase_start[WR_PHASE_COUNT], phase_ticks[WR_PHASE_COUNT];
static uint64_t uptime;
static unsigned boot_count, boot_saved, sequence;
static struct { const char *name; uint32_t tick; file_io_stats_t io; } boot[24];
static file_io_stats_t window_io;
static wr_profile_state first, latest;
static struct {
    uint32_t frames, engine, lcd, phase[WR_PHASE_COUNT], min, max;
    uint32_t bins[8], level, menu, demo, wipe, moved, changed;
} sample;
/* Histogram upper bounds: 33, 50, 67, 100, 150, 250, 500 ms, then infinity. */
static const uint32_t bounds[] = { 33, 50, 67, 100, 150, 250, 500 };
static char record[12288];
static unsigned used;
static int failed;
static uint32_t pending_flush;

/* Match the in-app timing window with independent emulator PC probes. */
void __attribute__((noinline)) doom_benchmark_start(void) { __asm__ volatile(""); }
void __attribute__((noinline)) doom_benchmark_end(void) { __asm__ volatile(""); }

static void add(const char *format, ...)
{
    if (failed) return;
    va_list args;
    va_start(args, format);
    int count = vsnprintf(record + used, sizeof(record) - used, format, args);
    va_end(args);
    if (count < 0 || (unsigned)count >= sizeof(record) - used) failed = 1;
    else used += count;
}

/* Close every batch, so the completed benchmark survives normal power-off.
   Never truncate previous runs. Stop tracing at 1 MiB or on an I/O error. */
static void flush(void)
{
    uint32_t begin = timer_get();
    unsigned long length = 0;
    int result = file_size(LOG_PATH, &length), handle = -1;
    if (!failed && length + used <= LOG_LIMIT) {
        if (result == FILE_ERROR_NO_FILE) handle = file_create(LOG_PATH, FILE_OPEN_WRITE);
        else if (result == FILE_ERROR_OK) {
            handle = file_open(LOG_PATH, FILE_OPEN_READ | FILE_OPEN_WRITE);
            if (handle >= 0 && file_lseek(handle, length) != FILE_ERROR_OK) {
                file_close(handle); handle = -1;
            }
        }
    }
    int ok = 0, wrote = -1, closed = -1;
    if (handle >= 0) {
        wrote = file_write(handle, record, used);
        closed = file_close(handle);
        ok = wrote == (int)used && closed == FILE_ERROR_OK;
    }
    pending_flush = (uint32_t)timer_get() - begin;
    if (!ok) {
        debug_printf("Doom performance log failed: stat=%d size=%lu buffered=%u format=%d handle=%d write=%d close=%d\n",
                     result, length, used, failed, handle, wrote, closed);
        wr_profile_enabled = 0;
        file_profile(NULL, false);
        bench = 0;
        wr_video_status("LOG FAILED - card full?");
    }
    used = 0;
    watchdog(WATCHDOG_KEY);
}

static void io_delta(const file_io_stats_t *a, const file_io_stats_t *b)
{
#define D(f) U((uint32_t)(b->f - a->f))
    add(" reads=%lu sectors=%lu read_us=%lu read_errors=%lu dma32=%lu dma8=%lu"
        " dma_wait_us=%lu bypass=%lu timeouts=%lu dma_errors=%lu dma_bits=%lu dma_disabled=%lu\n",
        D(read_calls), D(read_sectors), US(b->read_ticks - a->read_ticks), D(read_errors),
        D(dma32_bytes), D(dma8_bytes), US(b->dma_wait_ticks - a->dma_wait_ticks),
        D(dma_bypass_bytes), D(dma_timeouts), D(dma_errors), U(b->dma_bits), U(b->dma_disabled));
#undef D
}

void wr_profile_init(int argc, char **argv)
{
    app_start = last_end = (uint32_t)timer_get();
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-wrbench")) bench = 1;
        if (!strcmp(argv[i], "-wrtrace")) continuous = 1;
    }
    unsigned long size;
    wr_profile_enabled = bench || continuous || file_size("0:/doomlog.on", &size) == FILE_ERROR_OK;
    if (!wr_profile_enabled) return;
    file_profile(NULL, true);
    wr_profile_boot("app");
    add("DOOMPERF v=1 build=%s mode=%s ticks_per_us=60\n",
        WR_BUILD_ID, bench ? "benchmark" : "play");
    add("ARGS");
    for (int i = 0; i < argc; ++i) add(" %s", argv[i]);
    add("\nCLOCK kernel_epoch_raw=%lu scope=excludes_FLASH_and_kernel_load epoch_wrap_ms=71582\n",
        U(app_start));
#ifdef WR_C33
    add("REGS clk=%08lx gate=%08lx sdram_ctl=%08lx sdram_ref=%08lx sdram_app=%08lx\n",
        U(REG_CMU_CLKCNTL), U(REG_CMU_GATEDCLK1), U(REG_SDRAMC_CTL),
        U(REG_SDRAMC_REF), U(REG_SDRAMC_APP));
#endif
}

int wr_profile_locked(void) { return bench > 0 && bench < 4; }

void wr_profile_boot(const char *name)
{
    if (!wr_profile_enabled || boot_saved || boot_count == 24) return;
    boot[boot_count].name = name;
    boot[boot_count].tick = (uint32_t)timer_get();
    file_profile(&boot[boot_count++].io, true);
}

static void save_boot(const char *stage)
{
    wr_profile_boot(stage);
    boot_saved = 1;
    for (unsigned i = 0; i < boot_count; ++i) {
        add("BOOT stage=%s app_us=%lu elapsed_us=%lu", boot[i].name,
            US(boot[i].tick - app_start), i ? US(boot[i].tick - boot[i-1].tick) : 0UL);
        io_delta(&boot[i ? i-1 : 0].io, &boot[i].io);
    }
    for (unsigned i = 0; i < 8; ++i) {
        file_io_stats_t io, zero = {0};
        unsigned long begin, end;
        int kind = file_boot_profile(i, &io, &begin, &end);
        if (!kind) break;
        add("KERNELBOOT seq=%u kind=%d begin_raw=%lu elapsed_us=%lu", i, kind, begin, US(end - begin));
        io_delta(&zero, &io);
    }
    add("END_BOOT\n");
}

static void reset_window(uint32_t tick, const wr_profile_state *state)
{
    memset(&sample, 0, sizeof(sample));
    sample.min = UINT32_MAX;
    window_start = tick;
    first = *state;
    file_profile(&window_io, true);
}

void wr_profile_begin(void)
{
    frame_start = (uint32_t)timer_get();
    memset(phase_ticks, 0, sizeof(phase_ticks));
}
void wr_profile_engine_done(void) { engine_end = (uint32_t)timer_get(); }
void wr_profile_phase_begin(unsigned p) { phase_start[p] = (uint32_t)timer_get(); }
void wr_profile_phase_end(unsigned p) { phase_ticks[p] += (uint32_t)timer_get() - phase_start[p]; }

static void report(uint32_t end, const char *kind)
{
    file_io_stats_t io;
    file_profile(&io, true);
    add("%s seq=%u app_ms=%lu elapsed_us=%lu frames=%lu engine_us=%lu lcd_us=%lu"
        " bsp_us=%lu planes_us=%lu masked_us=%lu min_us=%lu max_us=%lu"
        " level=%lu menu=%lu demo=%lu wipe=%lu moved=%lu changed=%lu"
        " bins=%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu\n",
        kind, sequence++, U(uptime / 60000), US(end - window_start), U(sample.frames),
        US(sample.engine), US(sample.lcd), US(sample.phase[0]), US(sample.phase[1]), US(sample.phase[2]),
        US(sample.min), US(sample.max), U(sample.level), U(sample.menu), U(sample.demo),
        U(sample.wipe), U(sample.moved), U(sample.changed),
        U(sample.bins[0]), U(sample.bins[1]), U(sample.bins[2]), U(sample.bins[3]),
        U(sample.bins[4]), U(sample.bins[5]), U(sample.bins[6]), U(sample.bins[7]));
    add("STATE episode=%u map=%u skill=%u detail=%u width=%u height=%u"
        " tic_start=%u tic_end=%u level_start=%u level_end=%u x=%d y=%d angle=%u health=%d\n",
        latest.episode, latest.map, latest.skill, latest.detail, latest.width, latest.height,
        first.gametic, latest.gametic, first.leveltime, latest.leveltime,
        latest.x, latest.y, latest.angle, latest.health);
    add("IO"); io_delta(&window_io, &io);
}

void wr_profile_end(const wr_profile_state *state)
{
    uint32_t end = (uint32_t)timer_get();
    uint32_t interval = end - last_end;
    uptime += interval;
    last_end = end;
    latest = *state;
    if (!boot_saved) {
        save_boot("first_frame");
        /* A benchmark persists boot + warmup + result in one batch after
           measurement. Avoid a slow first-file allocation before warmup. */
        if (!bench) flush();
        if (!wr_profile_enabled) return;
        reset_window(end, state);
        if (bench) wr_video_status("BENCH: wait for warmup");
        return;
    }
    if (pending_flush) {
        add("FLUSH write_close_us=%lu\n", US(pending_flush));
        pending_flush = 0;
    }
    if (bench == 1) {
        if (state->state == 0 && !state->wipe) {
            bench = 2;
            reset_window(end, state);
            wr_video_status("BENCH: warming up (5s)");
        }
        return;
    }
    ++sample.frames;
    sample.engine += engine_end - frame_start;
    sample.lcd += end - engine_end;
    for (unsigned i = 0; i < WR_PHASE_COUNT; ++i) sample.phase[i] += phase_ticks[i];
    if (interval < sample.min) sample.min = interval;
    if (interval > sample.max) sample.max = interval;
    unsigned bin = 0;
    while (bin < 7 && interval > bounds[bin] * 60000U) ++bin;
    ++sample.bins[bin];
    sample.level += state->state == 0;
    sample.menu += !!state->menu;
    sample.demo += !!state->demo;
    sample.wipe += !!state->wipe;
    sample.moved += state->x != first.x || state->y != first.y || state->angle != first.angle;
    sample.changed += state->state != first.state || state->episode != first.episode ||
        state->map != first.map || state->detail != first.detail || state->width != first.width ||
        state->height != first.height || state->skill != first.skill;
    uint32_t duration = bench == 3 ? 10U * HZ : 5U * HZ;
    if (end - window_start < duration) return;
    if (bench == 2) {
        /* Freeze the scene boundary; no formatting, serial or SD writes
           between this boundary and the final benchmark frame. */
        report(end, "WARMUP");
        wr_video_status("BENCH: measuring (10s)");
        bench = 3;
        uint32_t start = (uint32_t)timer_get();
        uptime += start - last_end;
        reset_window(start, state);
        last_end = window_start;
        doom_benchmark_start();
        return;
    }
    int completed = bench == 3;
    if (completed) doom_benchmark_end();
    report(end, bench == 3 ? "BENCH" : "WINDOW");
    if (bench == 3) {
        bench = 4;
        add("BENCH_DONE\n");
        debug_print("Doom benchmark complete\n");
    }
    flush();
    if (completed && wr_profile_enabled) wr_video_status(NULL);
    if (completed && !continuous) {
        /* The default benchmark hands control back with no periodic card
           writes or frame instrumentation. -wrtrace explicitly keeps it. */
        file_profile(NULL, false);
        wr_profile_enabled = 0;
    }
    /* Normal windows include the preceding flush in wall time. IO counters
       exclude logging itself; FLUSH separately records the SD write stall. */
    if (wr_profile_enabled) reset_window(end, state);
}

void wr_profile_finish(int code)
{
    if (!wr_profile_enabled) return;
    uint32_t now = (uint32_t)timer_get();
    uptime += now - last_end;
    if (!boot_saved) save_boot("abort");
    if (!wr_profile_enabled) return;
    if (sample.frames) report(now, "PARTIAL");
    add("EXIT code=%d\n", code);
    flush();
    file_profile(NULL, false);
    wr_profile_enabled = 0;
}
