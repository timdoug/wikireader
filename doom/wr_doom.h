/* WikiReader Doom adapter, GPL-3.0-or-later. */
#ifndef WR_DOOM_H
#define WR_DOOM_H
#include <stdint.h>

enum {
    WR_ENTER = 13, WR_ESCAPE = 27, WR_SPACE = 32, WR_TAB = 9,
    WR_FIRE = 0x9d, WR_LEFT = 0xac, WR_UP = 0xad,
    WR_RIGHT = 0xae, WR_DOWN = 0xaf, WR_SHIFT = 0xb6,
    WR_WIDTH = 240, WR_HEIGHT = 180, WR_STRIDE = 32
};

void wr_engine_init(int argc, char **argv);
int wr_engine_step(void);
void wr_engine_key(int key, int down);
int wr_engine_menu(void);
void wr_engine_weapon(void);
void wr_engine_quit(void);
const unsigned char *wr_engine_frame(void);
const unsigned char *wr_engine_palette(void);

void wr_platform_init(void);
void wr_console_init(int argc, char **argv);
int wr_console_verbose(void);
uint32_t wr_milliseconds(void);
void wr_gettime(int *sec, int *usec);
void wr_print(const char *text);
void wr_error(const char *text);
void *wr_malloc(int size);
void wr_free(void *ptr);
void *wr_open(const char *name, const char *mode);
void wr_close(void *handle);
int wr_read(void *handle, void *buf, int count);
int wr_write(void *handle, const void *buf, int count);
int wr_seek(void *handle, int offset, int origin);
int wr_tell(void *handle);
int wr_eof(void *handle);
char *wr_getenv(const char *name);
void wr_exit(int code) __attribute__((noreturn));

void wr_video_init(void);
void wr_video_draw(const unsigned char *source, const unsigned char *palette);
void wr_controls_poll(void);
void wr_controls_frame_done(void);
void wr_controls_event(int type, int code, int x, int y);
#endif
