/* Immutable Doom light tables, GPL-3.0-or-later. */
#ifndef WR_RENDER_CACHE_H
#define WR_RENDER_CACHE_H
typedef struct {
    const unsigned char *source;
    unsigned char pixels[256];
} wr_light_cache;

static inline const unsigned char *wr_light_map(wr_light_cache *cache,
                                               const unsigned char *source)
{
    if (cache->source != source) {
        for (unsigned i = 0; i < 256; ++i) cache->pixels[i] = source[i];
        cache->source = source;
    }
    return cache->pixels;
}
#endif
