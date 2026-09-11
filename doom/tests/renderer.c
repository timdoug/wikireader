#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../engine.c"

static unsigned bytes_read;
void wr_print(const char *s) { fputs(s, stderr); }
void *wr_malloc(int n) { return malloc(n); }
void wr_free(void *p) { free(p); }
void *wr_open(const char *n, const char *m) { return fopen(n, m); }
void wr_close(void *p) { fclose(p); }
int wr_read(void *p, void *b, int n) {
    int count = (int)fread(b, 1, n, p); bytes_read += count; return count;
}
int wr_write(void *p, const void *b, int n) { return (int)fwrite(b, 1, n, p); }
int wr_seek(void *p, int n, int o) { return fseek(p, n, o); }
int wr_tell(void *p) { return (int)ftell(p); }
int wr_eof(void *p) { return feof(p); }
char *wr_getenv(const char *n) { return getenv(n); }
void wr_gettime(int *s, int *u) { *s = 1; *u = 0; }
void wr_exit(int code) { exit(code); }

static void check_flat_cache(void)
{
    unsigned data[1024], other[1024];
    unsigned char top[64] = {0}, bottom[64];
    memset(bottom, 63, sizeof(bottom));
    for (unsigned i = 0; i < 1024; ++i) {
        data[i] = i * 1664525u + 1013904223u;
        other[i] = ~data[i];
    }
    flat_lump = -1;
    const unsigned char *cached = cache_flat(7, (const unsigned char *)data, 64, top, bottom);
    assert(cached != (const unsigned char *)data);
    assert(!memcmp(cached, data, sizeof(data)));
    assert(cache_flat(7, (const unsigned char *)data, 64, top, bottom) == cached);
    /* Tiny planes and holes do not evict a useful texture; a cached texture
       can still be used for small spans without another admission check. */
    memset(bottom, 0, sizeof(bottom));
    assert(cache_flat(8, (const unsigned char *)other, 64, top, bottom) == (const unsigned char *)other);
    assert(flat_lump == 7);
    assert(cache_flat(7, (const unsigned char *)data, 64, top, bottom) == cached);
    memset(top, 255, sizeof(top)); memset(bottom, 255, sizeof(bottom));
    assert(!cache_large_flat(64, top, bottom));
    memset(top, 0, sizeof(top)); memset(bottom, 31, sizeof(bottom));
    assert(cache_large_flat(64, top, bottom));
    bottom[0] = 30;
    assert(!cache_large_flat(64, top, bottom));
    memset(bottom, 63, sizeof(bottom));
    /* A freed zone block can serve a different flat at the same address. */
    memcpy(data, other, sizeof(data));
    assert(!memcmp(cache_flat(8, (const unsigned char *)data, 64, top, bottom), other, sizeof(other)));
    for (unsigned i = 0; i < 1024; ++i) other[i] ^= 0x12345678u;
    assert(!memcmp(cache_flat(9, (const unsigned char *)other, 64, top, bottom), other, sizeof(other)));
    assert(!memcmp(cache_flat(8, (const unsigned char *)data, 64, top, bottom), data, sizeof(data)));
    /* A new engine session invalidates the previous WAD's lump identities. */
    flat_lump = -1;
    assert(!memcmp(cache_flat(8, (const unsigned char *)other, 64, top, bottom), other, sizeof(other)));
    flat_lump = -1;
}

static void check_sprite_headers(void)
{
    unsigned char data[4113], full[2048], prefix[16];
    for (unsigned i = 0; i < sizeof(data); ++i) data[i] = i * 31u;
    const short a[4] = {64, 128, -7, 99}, b[4] = {23, 41, 12, -3};
    memcpy(data+17, a, 8); memcpy(data+2065, b, 8);
    FILE *file = tmpfile(); assert(file);
    assert(fwrite(data, 1, sizeof(data), file) == sizeof(data));
    lumpinfo_t lumps[4] = {0};
    memcpy(lumps[0].name, "S_START", 8);
    memcpy(lumps[3].name, "S_END", 6);
    for (int i = 1; i <= 2; ++i) {
        lumps[i].handle = file; lumps[i].position = 17+(i-1)*2048; lumps[i].size = 2048;
    }
    numlumps = 4; lumpinfo = lumps;
    doom_set_print(wr_print); doom_set_malloc(wr_malloc, wr_free);
    doom_set_file_io(wr_open, wr_close, wr_read, wr_write, seek_file, wr_tell, wr_eof);
    memset(prefix, 0xee, sizeof(prefix)); bytes_read = 0;
    W_ReadLumpPrefix(1, prefix, 8);
    assert(bytes_read == 8 && !memcmp(prefix, a, 8));
    for (int i = 8; i < 16; ++i) assert(prefix[i] == 0xee);
    Z_Init(); bytes_read = 0;
    R_InitSpriteLumps();
    assert(numspritelumps == 2 && bytes_read == 16);
    assert(spritewidth[0] == 64*FRACUNIT && spritewidth[1] == 23*FRACUNIT);
    assert(spriteoffset[0] == -7*FRACUNIT && spriteoffset[1] == 12*FRACUNIT);
    assert(spritetopoffset[0] == 99*FRACUNIT && spritetopoffset[1] == -3*FRACUNIT);
    W_ReadLump(2, full); assert(!memcmp(full, data+2065, sizeof(full)));
    assert(bytes_read == 16+sizeof(full));
    fclose(file); free(mainzone); mainzone = NULL; lumpinfo = NULL; numlumps = 0;
}

static void check_texture_directories(void)
{
    /* Two overlapping wall patches, stored at unaligned WAD offsets.
       Metadata-only lookup must equal lookup from the complete images. */
    unsigned char data[2048] = {0};
    for (int p = 0; p < 2; ++p) {
        unsigned char *patch = data + 17 + p * 1024;
        short header[4] = {8, 4, 0, 0}; memcpy(patch, header, 8);
        for (int x = 0; x < 8; ++x) {
            int offset = 40 + x * 9; memcpy(patch + 8 + x * 4, &offset, 4);
            patch[offset] = 0; patch[offset + 1] = 4;
            for (int y = 0; y < 4; ++y) patch[offset + 3 + y] = 1 + p * 40 + x * 4 + y;
            patch[offset + 8] = 255;
        }
    }
    FILE *file = tmpfile(); assert(file);
    assert(fwrite(data, 1, sizeof(data), file) == sizeof(data));
    lumpinfo_t lumps[3] = {0}; void *cache[3] = {0}; patch_t *directories[3] = {0};
    for (int i = 1; i < 3; ++i) {
        lumps[i].handle = file; lumps[i].position = 17 + (i-1)*1024; lumps[i].size = 112;
    }
    numlumps = 3; lumpinfo = lumps; lumpcache = cache; texturepatchdirs = directories;
    texture_t *texture = calloc(1, sizeof(*texture) + sizeof(texpatch_t)); assert(texture);
    texture->width = 12; texture->height = 4; texture->patchcount = 2;
    texture->patches[0].patch = 1; texture->patches[1].patch = 2; texture->patches[1].originx = 4;
    texture_t *list[] = {texture}; short columns[12], expected_columns[12];
    unsigned short offsets[12], expected_offsets[12];
    short *column_list[] = {columns}; unsigned short *offset_list[] = {offsets};
    byte *composites[1] = {0}; int sizes[1] = {0};
    textures = list; texturecolumnlump = column_list; texturecolumnofs = offset_list;
    texturecomposite = composites; texturecompositesize = sizes;
    Z_Init(); bytes_read = 0;
    R_GenerateLookup(0);
    assert(bytes_read == 2 * (8 + 40) && !cache[1] && !cache[2]);
    assert(sizes[0] == 16);
    memcpy(expected_columns, columns, sizeof(columns)); memcpy(expected_offsets, offsets, sizeof(offsets));
    R_GenerateLookup(0); assert(bytes_read == 96); /* shared prefixes stay cached */
    for (int i = 1; i < 3; ++i) free(directories[i]);
    texturepatchdirs = NULL;
    R_GenerateLookup(0);
    assert(bytes_read == 96 + 224 && cache[1] && cache[2]);
    assert(!memcmp(columns, expected_columns, sizeof(columns)));
    assert(!memcmp(offsets, expected_offsets, sizeof(offsets)) && sizes[0] == 16);
    R_GenerateComposite(0);
    for (int x = 0; x < 4; ++x)
        for (int y = 0; y < 4; ++y) assert(composites[0][x*4+y] == 41+x*4+y);
    fclose(file); free(mainzone); mainzone = NULL; free(texture);
    lumpinfo = NULL; lumpcache = NULL; numlumps = 0;
    textures = NULL; texturecolumnlump = NULL; texturecolumnofs = NULL;
    texturecomposite = NULL; texturecompositesize = NULL;
}

static void check_random_spans(void)
{
    _Alignas(4) unsigned char frame[320*200+4], expected[sizeof(frame)];
    _Alignas(4) unsigned char maps[257], source[4096];
    unsigned seed = 0x91734123;
#define NEXT() (seed = seed * 1664525u + 1013904223u)
    for (unsigned i = 0; i < sizeof(source); ++i) source[i] = NEXT() >> 24;
    for (unsigned i = 0; i < sizeof(maps); ++i) maps[i] = NEXT() >> 24;
    for (int n = 0; n < 1000; ++n) {
        /* Exercise word/byte light copies, even/odd spans and offset buffers. */
        const unsigned char *map = maps + (n & 1);
        unsigned char *base = frame + ((n & 2) ? 2 : 0);
        memset(frame, 0xa5, sizeof(frame)); memset(expected, 0xa5, sizeof(expected));
        for (int y = 0; y < 200; ++y) ylookup[y] = base+y*320;
        for (int x = 0; x < 320; ++x) columnofs[x] = x;
        ds_y = NEXT() % 200; ds_x1 = NEXT() % 160;
        ds_x2 = ds_x1 + NEXT() % (160-ds_x1);
        ds_xfrac = NEXT(); ds_yfrac = NEXT(); ds_xstep = NEXT(); ds_ystep = NEXT();
        ds_source = source; ds_colormap = map;
        unsigned xf = ds_xfrac, yf = ds_yfrac;
        for (int x = ds_x1; x <= ds_x2; ++x) {
            unsigned pixel = map[source[((yf >> 10) & 4032) | ((xf >> 16) & 63)]];
            unsigned i = (unsigned)(base-frame) + ds_y*320+x*2;
            expected[i] = expected[i+1] = pixel;
            xf += (unsigned)ds_xstep; yf += (unsigned)ds_ystep;
        }
        R_DrawSpanLow(); assert(!memcmp(frame, expected, sizeof(frame)));
    }
#undef NEXT
    /* Caller-owned palette addresses must not outlive the test. */
    column_light.source = span_light.source = NULL;
}

int main(void)
{
    const int values[] = {0, 1, -1, 65536, -65536, 0x12345678, -0x12345678,
                          2147483647, -2147483647-1};
    for (unsigned i = 0; i < sizeof(values)/sizeof(*values); ++i)
        for (unsigned j = 0; j < sizeof(values)/sizeof(*values); ++j) {
            if (!values[j]) continue;
            long long expected = (long long)values[i] * 65536 / values[j];
            if (expected >= -2147483648LL && expected <= 2147483647LL)
                assert(FixedDiv2(values[i], values[j]) == expected);
        }
    unsigned random = 12345;
    for (unsigned i = 0; i < 10000; ++i) {
        random = random * 1664525u + 1013904223u;
        int a = (int)random;
        random = random * 1664525u + 1013904223u;
        int b = (int)random;
        if (!b) continue;
        long long expected = (long long)a * 65536 / b;
        if (expected >= -2147483648LL && expected <= 2147483647LL)
            assert(FixedDiv2(a, b) == expected);
        unsigned n = (unsigned)a, d = (unsigned)b;
        if ((n >> 16) < d)
            assert(FixedDivUnsigned(n, d) == (unsigned)(((unsigned long long)n << 16) / d));
    }
    _Alignas(2) unsigned char frame[320*200];
    unsigned char source[4096], palette[256];
    for (int i = 0; i < 256; ++i) palette[i] = i;
    for (int i = 0; i < 4096; ++i) source[i] = i % 64;
    for (int y = 0; y < 200; ++y) ylookup[y] = frame+y*320;
    for (int x = 0; x < 320; ++x) columnofs[x] = x;
    ds_source = source; ds_colormap = palette;
    ds_y = 50; ds_xfrac = 0; ds_yfrac = 0;
    ds_xstep = 65536; ds_ystep = 0;
    /* Full, partial and one-pair spans, including both screen edges. */
    const int spans[][2] = {{0,159},{4,8},{159,159},{0,0}};
    for (unsigned n = 0; n < sizeof(spans)/sizeof(*spans); ++n) {
        memset(frame, 0xee, sizeof(frame));
        ds_x1 = spans[n][0]; ds_x2 = spans[n][1];
        R_DrawSpanLow();
        assert(ds_x1 == spans[n][0] && ds_x2 == spans[n][1]);
        for (int i = 0; i < 320*200; ++i) {
            int begin = 50*320+spans[n][0]*2, end = 50*320+(spans[n][1]+1)*2;
            assert(frame[i] == (i >= begin && i < end ? ((i-begin)/2)%64 : 0xee));
        }
    }
    /* Texture rows, negative steps, wraparound, lighting and a view inset
       exercise more than a flat left-to-right span. */
    /* The fixture replaces COLORMAP in place; real WAD light tables are
       immutable for the engine's lifetime. */
    span_light.source = 0;
    for (int i = 0; i < 4096; ++i) source[i] = (i ^ (i >> 6)) & 255;
    for (int i = 0; i < 256; ++i) palette[i] = 255-i;
    for (int i = 0; i < 288; ++i) columnofs[i] = i+16;
    ds_y = 77; ds_x1 = 7; ds_x2 = 139;
    ds_xfrac = 0x7ffef123; ds_yfrac = -0x543210;
    ds_xstep = 0x61234; ds_ystep = -0x87654;
    memset(frame, 0xee, sizeof(frame));
    R_DrawSpanLow();
    for (int y = 0; y < 200; ++y) for (int x = 0; x < 320; ++x) {
        unsigned expected = 0xee;
        if (y == 77 && x >= 30 && x < 296) {
            unsigned step = (x-30)/2;
            unsigned xf = (unsigned)ds_xfrac + step * (unsigned)ds_xstep;
            unsigned yf = (unsigned)ds_yfrac + step * (unsigned)ds_ystep;
            expected = palette[source[((yf >> 10) & 4032) | ((xf >> 16) & 63)]];
        }
        assert(frame[y*320+x] == expected);
    }
    assert(ds_x1 == 7 && ds_x2 == 139);
    for (int i = 0; i < 256; ++i) palette[i] = i;
    for (int i = 0; i < 4096; ++i) source[i] = i % 64;
    for (int i = 0; i < 320; ++i) columnofs[i] = i;
    memset(frame, 0xee, sizeof(frame));
    dc_source = source; dc_colormap = palette;
    dc_x = 159; dc_yl = 2; dc_yh = 4; centery = 2;
    dc_texturemid = 0; dc_iscale = 65536;
    R_DrawColumnLow();
    assert(dc_x == 159);
    for (int y = 0; y < 200; ++y) for (int x = 0; x < 320; ++x)
        assert(frame[y*320+x] == (y >= 2 && y <= 4 && x >= 318 ? y-2 : 0xee));
    detailshift = 1;
    dc_translation = palette;
    memset(frame, 0xee, sizeof(frame));
    R_DrawTranslatedColumn();
    for (int y = 0; y < 200; ++y) for (int x = 0; x < 320; ++x)
        assert(frame[y*320+x] == (y >= 2 && y <= 4 && x >= 318 ? y-2 : 0xee));
    unsigned char light[7*256]; memset(light, 42, sizeof(light));
    colormaps = light; viewheight = 200;
    memset(frame, 0xee, sizeof(frame));
    R_DrawFuzzColumn();
    for (int y = 0; y < 200; ++y) for (int x = 0; x < 320; ++x)
        assert(frame[y*320+x] == (y >= 2 && y <= 4 && x >= 318 ? 42 : 0xee));
    check_sprite_headers();
    check_texture_directories();
    check_flat_cache();
    check_random_spans();
    puts("Doom renderer checks passed: fixed division, draw bounds, metadata, flat/light caches and random spans");
}
