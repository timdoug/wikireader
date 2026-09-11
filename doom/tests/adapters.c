/* Regression checks for the actual platform adapters, not engine mocks. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "grifo.h"
#include "wr_doom.h"

static uint32_t ticks;
static unsigned char fb[32 * 208 + 32];
static unsigned char key_state[256];
static int menu, weapon_changes;
static event_t queue[32];
static unsigned head, tail;
static unsigned char contents[65536];
static unsigned length, position;
static int exists;
static int map_calls, map_error, map_alloc_fail;
static unsigned long map_entries = 4, *attached_map;

unsigned long timer_get(void) { return ticks; }
void watchdog(watchdog_t key) { assert(key == WATCHDOG_KEY); }
void debug_print(const char *text) { (void)text; }
int debug_printf(const char *fmt, ...) { (void)fmt; return 0; }
void *memory_allocate(size_t n, const char *tag) {
    if (!strcmp(tag, "doomseek") && map_alloc_fail) return NULL;
    return malloc(n);
}
void memory_free(void *p, const char *tag) {
    (void)tag; assert(p != attached_map); free(p);
}
void panic(const char *fmt, ...) { (void)fmt; abort(); }
void chain(const char *cmd) { (void)cmd; abort(); }
void power_off(void) { abort(); }
event_item_t event_get(event_t *e) {
    if (head == tail) return EVENT_NONE;
    *e = queue[head++]; return e->item_type;
}
event_item_t event_wait(event_t *e, event_callback_t *cb, void *arg) {
    (void)e; (void)cb; (void)arg; abort();
}
void lcd_window_disable(void) {}
uint32_t *lcd_set_default_framebuffer(void) { return (uint32_t *)fb; }
void lcd_clear(lcd_colour_t c) { memset(fb, c ? 255 : 0, 32*208); }
uint8_t *lcd_get_framebuffer(void) { return fb; }
void lcd_move_to(int x, int y) { (void)x; (void)y; }
void lcd_line_to(int x, int y) { (void)x; (void)y; }
void lcd_at_xy(int x, int y) { (void)x; (void)y; }
void lcd_print(const char *s) { (void)s; }
int wr_engine_menu(void) { return menu; }
void wr_engine_weapon(void) { ++weapon_changes; }
void wr_engine_key(int key, int down) { key_state[key] = down; }
file_error_t file_size(const char *name, unsigned long *n) {
    assert(!strcmp(name, "/doom/test.dat"));
    *n = length; return exists ? FILE_ERROR_OK : FILE_ERROR_NO_FILE;
}
file_error_t file_open(const char *name, file_access_t mode) {
    assert(!strcmp(name, "/doom/test.dat"));
    if (mode & FILE_OPEN_TRUNCATE) { length = 0; exists = 1; }
    /* FatFs CREATE_NEW would wrongly reject overwriting an existing save. */
    assert(!(mode & FILE_OPEN_CREATE));
    position = 0; return exists ? 0 : FILE_ERROR_NO_FILE;
}
file_error_t file_close(int handle) { assert(handle == 0); attached_map = NULL; return 0; }
file_error_t file_fastseek(int handle, unsigned long *table, unsigned long entries) {
    assert(handle == 0); ++map_calls;
    table[0] = map_entries;
    if (entries < map_entries) return FILE_ERROR_NOT_ENOUGH_CORE;
    if (map_error) return FILE_ERROR_RW_ERROR;
    attached_map = table;
    return FILE_ERROR_OK;
}
ssize_t file_read(int h, void *p, size_t n) {
    assert(h == 0);
    if (n > length-position) n = length-position;
    memcpy(p, contents+position, n); position += n; return n;
}
ssize_t file_write(int h, void *p, size_t n) {
    assert(h == 0 && position+n <= sizeof(contents));
    memcpy(contents+position, p, n); position += n;
    if (position > length) length = position;
    return n;
}
file_error_t file_lseek(int h, unsigned long pos) {
    assert(h == 0 && pos <= sizeof(contents)); position = pos; return 0;
}
static void enqueue(int type, int code, int x, int y) {
    assert(tail < 32);
    event_t *e = &queue[tail++]; e->item_type = type;
    if (type == EVENT_BUTTON_DOWN || type == EVENT_BUTTON_UP) e->button.code = code;
    else { e->touch.x = x; e->touch.y = y; }
}

int main(void)
{
    ticks = 0xffff0000U;
    wr_platform_init();
    int sec, usec;
    ticks += 120000; /* wrap */
    wr_gettime(&sec, &usec); assert(sec == 1 && usec == 2000);
    for (int i = 0; i < 100; ++i) { ticks += 60000000; wr_gettime(&sec, &usec); }
    assert(sec == 101 && usec == 2000);

    assert(!wr_open("test.dat", "rb"));
    void *f = wr_open("test.dat", "wb"); assert(f);
    assert(wr_write(f, "abcdef", 6) == 6 && wr_tell(f) == 6); wr_close(f);
    f = wr_open("test.dat", "rb");
    char buf[16];
    assert(wr_seek(f, -2, 2) == 0 && wr_tell(f) == 4);
    assert(wr_read(f, buf, 4) == 2 && wr_eof(f) && !memcmp(buf, "ef", 2));
    assert(wr_seek(f, -1, 0) == -1);
    assert(wr_seek(f, 1, 0) == 0 && !wr_eof(f));
    assert(wr_seek(f, 1, 1) == 0 && wr_tell(f) == 2);
    wr_close(f);
    f = wr_open("test.dat", "wb"); assert(f && length == 0); wr_close(f);
    assert(map_calls == 0);

    /* Large read-only files get a persistent map on their first seek.
       Exercise contiguous/fragmented files and optional-cache failures. */
    length = sizeof(contents);
    for (unsigned i = 0; i < length; ++i) contents[i] = i * 31u;
    f = wr_open("test.dat", "rb"); wr_close(f); /* existence probe */
    assert(map_calls == 0);
    for (int mode = 0; mode < 5; ++mode) {
        map_calls = 0;
        map_entries = mode == 1 ? 512 : mode == 4 ? 65537 : 4;
        map_error = mode == 2; map_alloc_fail = mode == 3;
        f = wr_open("test.dat", "rb"); assert(f);
        assert(wr_seek(f, 0, 0) == 0 && map_calls == 0);
        assert(wr_seek(f, 32765, 0) == 0 && wr_tell(f) == 32765);
        assert(map_calls == (mode == 3 ? 0 : mode == 1 ? 2 : 1));
        assert((attached_map != NULL) == (mode < 2));
        assert(wr_read(f, buf, sizeof(buf)) == sizeof(buf));
        assert(!memcmp(buf, contents+32765, sizeof(buf)));
        assert(wr_seek(f, 8, 0) == 0);
        assert(wr_read(f, buf, sizeof(buf)) == sizeof(buf));
        assert(!memcmp(buf, contents+8, sizeof(buf)));
        assert(wr_seek(f, -4, 2) == 0 && wr_read(f, buf, sizeof(buf)) == 4 && wr_eof(f));
        assert(!memcmp(buf, contents+length-4, 4));
        assert(map_calls == (mode == 3 ? 0 : mode == 1 ? 2 : 1));
        wr_close(f);
    }
    map_calls = map_error = map_alloc_fail = 0;

    /* A tap entirely queued during one frame must still fire. */
    enqueue(EVENT_BUTTON_DOWN, BUTTON_RANDOM, 0, 0);
    enqueue(EVENT_BUTTON_UP, BUTTON_RANDOM, 0, 0);
    wr_controls_frame_done(); wr_controls_poll(); assert(key_state[WR_FIRE]);
    /* Transition frames do not run simulation: keep the tap latched. */
    wr_controls_poll(); assert(key_state[WR_FIRE]);
    wr_controls_frame_done(); wr_controls_poll(); assert(!key_state[WR_FIRE]);
    enqueue(EVENT_TOUCH_DOWN, 0, 20, 20);
    enqueue(EVENT_BUTTON_DOWN, BUTTON_RANDOM, 0, 0);
    wr_controls_frame_done(); wr_controls_poll(); assert(key_state[WR_LEFT] && key_state[WR_UP] && key_state[WR_FIRE]);
    enqueue(EVENT_TOUCH_MOTION, 0, 220, 150);
    enqueue(EVENT_BUTTON_UP, BUTTON_RANDOM, 0, 0);
    wr_controls_frame_done(); wr_controls_poll(); assert(!key_state[WR_LEFT] && !key_state[WR_UP] && key_state[WR_RIGHT] && key_state[WR_DOWN]);
    enqueue(EVENT_TOUCH_UP, 0, 0, 0); wr_controls_frame_done(); wr_controls_poll();
    assert(!key_state[WR_RIGHT] && !key_state[WR_DOWN]);
    menu = 1;
    enqueue(EVENT_BUTTON_DOWN, BUTTON_RANDOM, 0, 0); wr_controls_frame_done(); wr_controls_poll();
    assert(key_state[WR_ENTER] && !key_state[WR_FIRE]);
    enqueue(EVENT_BUTTON_UP, BUTTON_RANDOM, 0, 0); wr_controls_frame_done(); wr_controls_poll();
    assert(!key_state[WR_ENTER]);

    static unsigned char pixels[320*200], palette[768];
    wr_video_init();
    memset(fb, 0xa5, sizeof(fb));
    wr_video_draw(pixels, palette); /* palette zero = all black */
    for (int y = 0; y < 180; ++y) {
        for (int x = 0; x < 30; ++x) assert(fb[y*32+x] == 255);
        assert(fb[y*32+30] == 0xa5 && fb[y*32+31] == 0xa5);
    }
    for (unsigned i = 180*32; i < sizeof(fb); ++i) assert(fb[i] == 0xa5);
    memset(palette, 255, sizeof(palette));
    wr_video_draw(pixels, palette);
    for (int y = 0; y < 180; ++y) for (int x = 0; x < 30; ++x) assert(!fb[y*32+x]);
    /* Left/right and top/bottom sampling endpoints, without cropping HUD. */
    memset(palette, 0, sizeof(palette));
    palette[3] = palette[4] = palette[5] = 255;
    pixels[198*320+318] = 1;
    wr_video_draw(pixels, palette);
    assert(fb[179*32+29] == 254 && fb[178*32+29] == 255);
    /* Compare every pixel with the original scalar conversion across all
       sampling/dither phases, palette changes, and the cached-palette path. */
    const unsigned char bayer[16] = {
        0,8,2,10, 12,4,14,6, 3,11,1,9, 15,7,13,5
    };
    unsigned random = 12345;
    for (int pass = 0; pass < 3; ++pass) {
        for (unsigned i = 0; i < sizeof(pixels); ++i) {
            random = random * 1664525u + 1013904223u;
            pixels[i] = random >> 24;
        }
        if (pass != 2) for (unsigned i = 0; i < sizeof(palette); ++i) {
            random = random * 1664525u + 1013904223u;
            palette[i] = pass ? random >> 24 : i / 3;
        }
        memset(fb, 0xa5, sizeof(fb));
        wr_video_draw(pixels, palette);
        for (unsigned y = 0; y < 180; ++y) {
            for (unsigned x = 0; x < 240; ++x) {
                unsigned i = pixels[(y * 10 / 9) * 320 + x * 4 / 3];
                unsigned gray = (77*palette[i*3] + 150*palette[i*3+1] + 29*palette[i*3+2]) >> 8;
                gray = gray * (510-gray) / 255;
                unsigned black = gray < bayer[(y%4)*4 + x%4]*16 + 8;
                assert(((fb[y*32+x/8] >> (7-x%8)) & 1) == black);
            }
            assert(fb[y*32+30] == 0xa5 && fb[y*32+31] == 0xa5);
        }
        for (unsigned i = 180*32; i < sizeof(fb); ++i) assert(fb[i] == 0xa5);
    }
    puts("Doom adapter checks passed: timer wrap, files, short taps, movement, menu, display bounds");
    return 0;
}
