/* Immutable Doom light tables, GPL-3.0-or-later. */
#ifndef WR_RENDER_CACHE_H
#define WR_RENDER_CACHE_H
#include <stdint.h>
typedef struct {
    const unsigned char *source;
    unsigned char pixels[256];
} wr_light_cache;

static inline const unsigned char *wr_light_map(wr_light_cache *cache,
                                               const unsigned char *source)
{
    if (cache->source != source) {
        /* WAD colormaps and the cache are word aligned. Copy words in
           groups of four; retain the byte fallback for unaligned sources. */
        if (!((uintptr_t)source & 3)) {
            const uint32_t *in = (const uint32_t *)source;
            uint32_t *out = (uint32_t *)cache->pixels;
            for (unsigned i = 0; i < 64; i += 4) {
                out[i] = in[i]; out[i+1] = in[i+1];
                out[i+2] = in[i+2]; out[i+3] = in[i+3];
            }
        } else {
            for (unsigned i = 0; i < 256; ++i) cache->pixels[i] = source[i];
        }
        cache->source = source;
    }
    return cache->pixels;
}
#endif
