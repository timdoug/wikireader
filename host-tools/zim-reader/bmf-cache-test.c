/* Compare lazy glyph loading with full records, including prefix boundaries. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <grifo.h>
#include "bmf.h"

static unsigned char *files[2];
static const unsigned counts[2] = {600, 8192};
static unsigned long positions[2];

static unsigned long bytes(int fd)
{
    return sizeof(font_bmf_header) + counts[fd] * sizeof(charmetric_bmf);
}

file_error_t file_open(const char *name, file_access_t access)
{
    int fd = !strcmp(name, "large");
    assert(access == FILE_OPEN_READ);
    positions[fd] = 0;
    return fd;
}
file_error_t file_size(const char *name, unsigned long *size)
{
    *size = bytes(!strcmp(name, "large"));
    return FILE_ERROR_OK;
}
file_error_t file_lseek(int fd, unsigned long offset)
{
    assert(offset <= bytes(fd));
    positions[fd] = offset;
    return FILE_ERROR_OK;
}
ssize_t file_read(int fd, void *buffer, size_t length)
{
    if (length > bytes(fd) - positions[fd]) length = bytes(fd) - positions[fd];
    memcpy(buffer, files[fd] + positions[fd], length);
    positions[fd] += length;
    return length;
}
file_error_t file_fastseek(int fd, unsigned long *table, unsigned long entries)
{
    (void)fd;
    assert(entries >= 4);
    table[0] = 4;
    return FILE_ERROR_OK;
}
void *memory_allocate(size_t size, const char *tag)
{
    void *p = malloc(size);
    (void)tag;
    assert(p);
    memset(p, 0xa5, size); /* expose reads of records not initialized yet */
    return p;
}
void memory_free(void *p, const char *tag) { (void)tag; free(p); }
void panic(const char *format, ...)
{
    (void)format;
    abort();
}
void fatal_error_print(const char *file, int line, const char *format, ...)
{
    (void)file; (void)line; (void)format;
    abort();
}

static const charmetric_bmf *record(int fd, unsigned code)
{
    return (const charmetric_bmf *)(files[fd] + sizeof(font_bmf_header)) + code;
}

static void check(pcffont_bmf_t *font, unsigned code)
{
    int fd = code >= counts[font->fd] ? 1 : font->fd;
    const charmetric_bmf *ref = record(fd, code);
    charmetric_bmf metric;
    bmf_bm_t *bitmap = NULL;
    int width;
    if (code > 256 && ref->width <= 0) ref = record(fd, 63);
    width = code == 32 || ref->width > 0 ? ref->widthDevice : 0;
    if ((code & 1) || code == 32) assert(bmf_char_width(code, font) == width);
    assert(pres_bmfbm(code, font, &bitmap, &metric) == 1);
    assert(!memcmp(&metric, ref, sizeof(metric)));
    assert(!!bitmap == (ref->width > 0));
    assert(bmf_char_width(code, font) == width);
}

int main(void)
{
    pcffont_bmf_t fonts[2] = {{0}, {0}};
    unsigned fd, code, i;
    static const unsigned eviction[] = {127,128,129,255,256,257,511,512,
                                       513,2176,4224,128,700,700,8191,128};
    for (fd = 0; fd < 2; ++fd) {
        font_bmf_header header = {20,16,4,0,63};
        files[fd] = calloc(1, bytes(fd));
        assert(files[fd]);
        memcpy(files[fd], &header, sizeof(header));
        for (code = 32; code < counts[fd]; ++code) {
            charmetric_bmf *r = (charmetric_bmf *)record(fd, code);
            if (code == 130 || code == 700) continue;
            r->width = code == 32 ? 0 : 1 + code % 12;
            r->height = 16;
            r->widthDevice = code == 32 ? 4 : r->width + 1;
            for (i = 0; i < sizeof(r->bitmap); ++i)
                r->bitmap[i] = (char)(code * 13 + i * 7);
        }
        fonts[fd].file = fd ? "large" : "small";
        fonts[fd].fd = load_bmf(&fonts[fd]);
        assert(fonts[fd].fd == (int)fd);
    }
    fonts[0].bPartialFont = 1;
    fonts[0].supplement_font = &fonts[1];
    assert(fonts[0].glyph_slots == 0 && fonts[1].glyph_slots > 0);
    for (fd = 0; fd < 2; ++fd) {
        for (code = 0; code < counts[1]; ++code) check(&fonts[fd], code);
        for (i = 0; i < sizeof(eviction) / sizeof(eviction[0]); ++i)
            check(&fonts[fd], eviction[i]);
        free(fonts[fd].charmetric);
        free(fonts[fd].glyph_cache);
        free(fonts[fd].link_map);
        free(files[fd]);
    }
    puts("PASS: BMF prefix boundaries, lazy glyphs, cache eviction and fallback");
    return 0;
}
