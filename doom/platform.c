/* Grifo file, clock and memory callbacks, GPL-3.0-or-later. */
#include <grifo.h>
#include <string.h>
#include "wr_doom.h"
#ifdef WR_C33
#include "profile.h"
#endif

typedef struct {
    int handle;
    uint32_t position, length;
    int eof, writable;
    int seek_map_attempted;
    unsigned long *seek_map;
} wr_file;

static uint32_t last_ticks, seconds, remainder;
static int verbose_console;

void wr_console_init(int argc, char **argv)
{
    verbose_console = 0;
    for (int i = 1; i < argc; ++i)
        if (!strcmp(argv[i], "-wrverbose")) verbose_console = 1;
}

int wr_console_verbose(void) { return verbose_console; }

void wr_platform_init(void) { last_ticks = timer_get(); }

void wr_gettime(int *sec, int *usec)
{
    uint32_t now = timer_get();
    uint32_t elapsed = now - last_ticks;
    last_ticks = now;
    /* Timer wraps every 71.6 seconds. Accumulate without a 64-bit divide. */
    seconds += elapsed / 60000000;
    remainder += elapsed % 60000000;
    if (remainder >= 60000000) { ++seconds; remainder -= 60000000; }
    *sec = (int)seconds + 1; /* PureDOOM uses zero as its uninitialized epoch. */
    *usec = (int)(remainder / 60);
    watchdog(WATCHDOG_KEY);
}

uint32_t wr_milliseconds(void)
{
    int sec, usec;
    wr_gettime(&sec, &usec);
    return (uint32_t)sec * 1000 + (unsigned)usec / 1000;
}

void wr_print(const char *text)
{
    if (verbose_console) debug_print(text);
    watchdog(WATCHDOG_KEY);
}

void wr_error(const char *text)
{
    /* Fatal diagnostics must survive quiet startup, including messages
       without an "Error:" prefix and errors raised after initialization. */
    debug_print(text);
    debug_print("\n");
    watchdog(WATCHDOG_KEY);
}

void *wr_malloc(int size)
{
    if (size <= 0) size = 1;
    void *result = memory_allocate((size_t)size, "doom");
    if (!result) panic("Doom: allocation failed (%d bytes)\n", size);
    return result;
}

void wr_free(void *ptr) { if (ptr) memory_free(ptr, "doom"); }

void *wr_open(const char *name, const char *mode)
{
    /* Keep configuration and saves beside the WAD, including Doom's bare
       "doomsav0.dsg" names. Absolute paths (e.g. -file) are retained. */
    char path[256];
    if (name[0] != '/' && !strchr(name, ':')) {
        if (strlen(name) + sizeof("/doom/") > sizeof(path)) return NULL;
        strcpy(path, "/doom/");
        strcat(path, name);
        name = path;
    }
    int writable = mode[0] == 'w';
    if (mode[0] != 'r' && !writable) return NULL;
    unsigned long size = 0;
    if (!writable && (file_size(name, &size) < 0 || size > 0x7fffffffUL))
        return NULL;
    int handle = file_open(name, writable ?
        FILE_OPEN_WRITE | FILE_OPEN_TRUNCATE : FILE_OPEN_READ);
    if (handle < 0) return NULL;
    wr_file *f = wr_malloc(sizeof(*f));
    f->handle = handle;
    f->position = 0;
    f->length = (uint32_t)size;
    f->eof = 0;
    f->writable = writable;
    f->seek_map_attempted = 0;
    f->seek_map = NULL;
    return f;
}

void wr_close(void *handle)
{
    if (!handle) return;
    wr_file *f = handle;
    file_close(f->handle);
    /* FatFs retains this pointer until the file is closed. */
    if (f->seek_map) memory_free(f->seek_map, "doomseek");
    wr_free(f);
}

static void prepare_seek_map(wr_file *f)
{
    /* WAD loading makes hundreds of backward seeks. Without a cluster
       map, FAT32 walks the allocation chain again for every one. Build
       the map once, on the first seek, so existence probes stay cheap. */
    f->seek_map_attempted = 1;
    unsigned long entries = 128;
    unsigned long *table = memory_allocate(entries * sizeof(*table), "doomseek");
    if (!table) return;
    int result = file_fastseek(f->handle, table, entries);
    if (result == FILE_ERROR_NOT_ENOUGH_CORE) {
        entries = table[0];
        memory_free(table, "doomseek");
        /* Bound optional memory use even on a heavily fragmented card. */
        if (entries <= 128 || entries > 65536) return;
        table = memory_allocate(entries * sizeof(*table), "doomseek");
        if (!table) return;
        result = file_fastseek(f->handle, table, entries);
    }
    if (result == FILE_ERROR_OK) f->seek_map = table;
    else memory_free(table, "doomseek"); /* Failed maps are detached by Grifo. */
    watchdog(WATCHDOG_KEY);
}

int wr_read(void *handle, void *buf, int count)
{
    if (!handle || count < 0) return -1;
    wr_file *f = handle;
    int n = file_read(f->handle, buf, (size_t)count);
    if (n >= 0) { f->position += n; f->eof = n < count; }
    watchdog(WATCHDOG_KEY);
    return n;
}

int wr_write(void *handle, const void *buf, int count)
{
    if (!handle || count < 0) return -1;
    wr_file *f = handle;
    int n = file_write(f->handle, (void *)buf, (size_t)count);
    if (n >= 0) {
        f->position += n;
        if (f->position > f->length) f->length = f->position;
    }
    watchdog(WATCHDOG_KEY);
    return n;
}

int wr_seek(void *handle, int offset, int origin)
{
    if (!handle || origin < 0 || origin > 2) return -1;
    wr_file *f = handle;
    int64_t pos = (int64_t)(origin == 1 ? f->position : origin == 2 ? f->length : 0) + offset;
    if (pos < 0 || pos > 0x7fffffff || (!f->writable && pos > f->length)) return -1;
    if ((uint32_t)pos != f->position) {
        if (!f->writable && f->length >= 65536 && !f->seek_map_attempted)
            prepare_seek_map(f);
        if (file_lseek(f->handle, (unsigned long)pos) < 0) return -1;
    }
    f->position = (uint32_t)pos;
    f->eof = 0;
    return 0;
}

int wr_tell(void *handle) { return handle ? (int)((wr_file *)handle)->position : -1; }
int wr_eof(void *handle) { return !handle || ((wr_file *)handle)->eof; }
char *wr_getenv(const char *name)
{
    if (!strcmp(name, "HOME") || !strcmp(name, "DOOMWADDIR")) return "/doom";
    return NULL;
}

void wr_exit(int code)
{
#ifdef WR_C33
    wr_profile_finish(code);
#endif
    debug_printf("Doom exited: %d\n", code);
    if (code) {
        lcd_set_default_framebuffer();
        lcd_clear(LCD_WHITE);
        lcd_print("Doom stopped.\nCheck the WAD in /doom.\n\nHistory: return to launcher");
        for (;;) {
            event_t e;
            if (event_wait(&e, NULL, NULL) == EVENT_BUTTON_DOWN && e.button.code == BUTTON_HISTORY)
                break;
        }
    }
    chain("init.app");
}
