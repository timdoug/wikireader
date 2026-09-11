/* Exercise persisted records, timing wrap, write stalls and failure paths. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "grifo.h"
#include "profile.h"

static uint32_t ticks = 0xffff0000U;
static char data[65536], status[128];
static unsigned length, pos, writes, closes, creates;
static int marker, short_write, close_error, seek_error, full;
static file_io_stats_t io;
unsigned long timer_get(void) { return ticks; }
void watchdog(watchdog_t key) { assert(key == WATCHDOG_KEY); }
void debug_print(const char *s) { (void)s; }
int debug_printf(const char *s, ...) { (void)s; return 0; }
void wr_video_status(const char *s) { snprintf(status, sizeof(status), "%s", s ? s : "controls"); }
void file_profile(file_io_stats_t *out, bool enabled) { (void)enabled; if (out) *out = io; }
int file_boot_profile(unsigned i, file_io_stats_t *out, unsigned long *begin, unsigned long *end)
{ (void)i; (void)out; (void)begin; (void)end; return 0; }
file_error_t file_size(const char *path, unsigned long *size)
{
    if (!strcmp(path, "0:/doomlog.on")) return marker ? FILE_ERROR_OK : FILE_ERROR_NO_FILE;
    assert(!strcmp(path, "0:/doomperf.log"));
    *size = full ? 1024 * 1024 : length;
    return length || full ? FILE_ERROR_OK : FILE_ERROR_NO_FILE;
}
file_error_t file_create(const char *path, file_access_t mode)
{ assert(!strcmp(path, "0:/doomperf.log") && mode == FILE_OPEN_WRITE && !length); ++creates; pos = 0; return 0; }
file_error_t file_open(const char *path, file_access_t mode)
{ assert(!strcmp(path, "0:/doomperf.log") && mode == (FILE_OPEN_READ | FILE_OPEN_WRITE)); pos = 0; return 0; }
file_error_t file_lseek(int h, unsigned long p)
{ assert(h == 0 && p == length); pos = p; return seek_error ? FILE_ERROR_RW_ERROR : FILE_ERROR_OK; }
ssize_t file_write(int h, void *buf, size_t n)
{
    assert(h == 0 && pos == length && length + n < sizeof(data));
    ++writes; ticks += 15000000; /* 250 ms card write, deliberately large. */
    if (short_write) --n;
    memcpy(data + pos, buf, n); length += n; pos += n; data[length] = 0;
    return n;
}
file_error_t file_close(int h) { assert(h == 0); ++closes; return close_error ? FILE_ERROR_RW_ERROR : FILE_ERROR_OK; }

static wr_profile_state state = { .state = 0, .episode = 1, .map = 1, .skill = 2,
    .detail = 1, .width = 160, .height = 168, .health = 100 };
static void frame(void)
{
    if (!wr_profile_enabled) { ticks += 3000000; return; }
    wr_profile_begin();
    const unsigned phase[] = {600000, 480000, 120000};
    for (unsigned i = 0; i < 3; ++i) {
        wr_profile_phase_begin(i); ticks += phase[i]; wr_profile_phase_end(i);
    }
    ticks += 1200000; wr_profile_engine_done();
    ticks += 600000; state.gametic += 2; state.leveltime += 2;
    wr_profile_end(&state);
}

static void run(int which)
{
    char *args[] = { "doom.app", "-wrbench", "-wrtrace" };
    if (which == 0) {
        wr_profile_init(1, args); assert(!wr_profile_enabled); return;
    }
    if (which == 8 || which == 9) {
        marker = which == 8;
        args[1] = "-wrtrace";
        wr_profile_init(which == 8 ? 1 : 2, args);
        assert(wr_profile_enabled && !wr_profile_locked());
        frame(); assert(writes == 1 && strstr(data, "stage=first_frame"));
        for (int i = 0; i < 150; ++i) frame();
        assert(writes > 1 && strstr(data, "WINDOW seq=") && !strstr(data, "BENCH seq="));
        wr_profile_finish(0); assert(strstr(data, "EXIT code=0"));
        return;
    }
    if (which == 10) {
        wr_profile_init(2, args);
        wr_profile_finish(1);
        assert(writes == 1 && strstr(data, "stage=abort"));
        assert(!strstr(data, "stage=first_frame") && strstr(data, "EXIT code=1"));
        return;
    }
    if (which == 2) short_write = 1;
    if (which == 3) full = 1;
    if (which == 4) close_error = 1;
    if (which == 5) seek_error = 1;
    if (which >= 3 && which <= 5) { strcpy(data, "PREVIOUS RUN\n"); length = strlen(data); }
    marker = which == 7; /* -wrbench still stops after completion with a marker. */
    wr_profile_init(which == 6 ? 3 : 2, args); assert(wr_profile_enabled && wr_profile_locked());
    ticks += 60000000; wr_profile_boot("engine_ready"); frame();
    /* Even the boot record stays in RAM until the benchmark is complete. */
    assert(!writes && !closes && !creates);
    state.wipe = 1;
    for (int i = 0; i < 150; ++i) { frame(); assert(!writes && wr_profile_locked()); }
    state.wipe = 0;
    while (!strstr(status, "measuring")) frame();
    for (int i = 0; i < 199; ++i) { frame(); assert(!writes && wr_profile_locked()); }
    frame();
    if (which >= 2 && which <= 5) {
        assert(!wr_profile_enabled && !wr_profile_locked());
        assert(strstr(status, "LOG FAILED"));
        if (which >= 3) { assert(!strncmp(data, "PREVIOUS RUN\n", 13)); assert(!creates); }
        if (which == 3 || which == 5) assert(!writes);
        return;
    }
    assert(writes == 1 && closes == 1 && creates == 1 && !wr_profile_locked());
    assert(strstr(data, "stage=engine_ready app_us=1000000")); /* wraps timer */
    unsigned before = writes;
    const char *bench = strstr(data, "BENCH seq="); assert(bench);
    assert(strstr(bench, "elapsed_us=10000000 frames=200 engine_us=8000000 lcd_us=2000000"));
    assert(strstr(bench, "bsp_us=2000000 planes_us=1600000 masked_us=400000"));
    assert(strstr(bench, "min_us=50000 max_us=50000 level=200 menu=0 demo=0 wipe=0 moved=0 changed=0"));
    assert(strstr(data, "BENCH_DONE"));
    /* Long sessions cross the 32-bit timer repeatedly; periodic flushes are
       included in live wall time, but excluded from engine/LCD phases. */
    for (int i = 0; i < 3000; ++i) frame();
    if (which == 6) {
        assert(wr_profile_enabled && writes > before);
        assert(strstr(data, "min_us=50000 max_us=300000"));
        assert(strstr(data, "FLUSH write_close_us=250000"));
    } else assert(!wr_profile_enabled && writes == before);
    wr_profile_finish(0); assert(!wr_profile_enabled && closes == writes);
    if (which == 6) assert(strstr(data, "EXIT code=0"));
}

int main(void)
{
    for (int i = 0; i < 11; ++i) {
        pid_t child = fork(); assert(child >= 0);
        if (!child) { run(i); exit(0); }
        int result; assert(waitpid(child, &result, 0) == child);
        assert(WIFEXITED(result) && WEXITSTATUS(result) == 0);
    }
    puts("profile: wrap, benchmark boundaries, persistence, overhead and I/O failures passed");
    return 0;
}
