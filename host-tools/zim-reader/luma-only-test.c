/* Verify that omitted chroma work leaves brightness, alpha and RGB intact. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "src/webp/decode.h"
#include "luma-only-fixtures.h"

#define STRIDE 256
#define PLANE (STRIDE * 64)
#define SENTINEL 0xa5

struct result {
    unsigned char pixels[4 * PLANE];
    int width, height;
};

static void decode(struct result *r, const unsigned char *data, size_t size,
                   WEBP_CSP_MODE mode, int luma, int crop, int scale,
                   int filtering, size_t chunk)
{
    WebPDecoderConfig config;
    WebPIDecoder *decoder;
    VP8StatusCode status = VP8_STATUS_SUSPENDED;
    size_t end;

    assert(WebPInitDecoderConfig(&config));
    assert(WebPGetFeatures(data, size, &config.input) == VP8_STATUS_OK);
    config.options.luma_only = luma;
    config.options.bypass_filtering = !filtering;
    config.options.dithering_strength = 63;
    config.options.use_cropping = crop;
    config.options.crop_left = 3; /* decoder snaps the origin to even */
    config.options.crop_top = 5;
    config.options.crop_width = 23;
    config.options.crop_height = 17;
    config.options.use_scaling = scale != 0;
    config.options.scaled_width = scale == 1 ? 13 : 47;
    config.options.scaled_height = scale == 1 ? 9 : 39;
    config.output.colorspace = mode;
    config.output.is_external_memory = 1;
    memset(r->pixels, SENTINEL, sizeof(r->pixels));
    if (WebPIsRGBMode(mode)) {
        config.output.u.RGBA.rgba = r->pixels;
        config.output.u.RGBA.stride = STRIDE;
        config.output.u.RGBA.size = sizeof(r->pixels);
    } else {
        WebPYUVABuffer *p = &config.output.u.YUVA;
        p->y = r->pixels;
        p->u = r->pixels + PLANE;
        p->v = r->pixels + 2 * PLANE;
        p->a = r->pixels + 3 * PLANE;
        p->y_stride = p->u_stride = p->v_stride = p->a_stride = STRIDE;
        p->y_size = p->u_size = p->v_size = p->a_size = PLANE;
    }
    decoder = WebPIDecode(NULL, 0, &config);
    assert(decoder != NULL);
    for (end = chunk; ; end += chunk) {
        if (end > size) end = size;
        status = WebPIUpdate(decoder, data, end);
        assert(status == VP8_STATUS_OK || status == VP8_STATUS_SUSPENDED);
        if (end == size) break;
    }
    assert(status == VP8_STATUS_OK);
    r->width = config.output.width;
    r->height = config.output.height;
    WebPIDelete(decoder);
    WebPFreeDecBuffer(&config.output);
}

int main(void)
{
    const unsigned char *data[] = {fixture0, fixture1, fixture2};
    const size_t sizes[] = {sizeof(fixture0), sizeof(fixture1), sizeof(fixture2)};
    const WEBP_CSP_MODE modes[] = {MODE_YUV, MODE_YUVA, MODE_RGBA};
    struct result full, luma;
    unsigned cases = 0;
    int f, m, crop, scale, filtering, incremental;
    for (f = 0; f < 3; ++f)
    for (m = f == 2 ? 2 : 0; m < 3; ++m)
    for (crop = 0; crop < 2; ++crop)
    for (scale = 0; scale < 3; ++scale)
    for (filtering = 0; filtering < 2; ++filtering)
    for (incremental = 0; incremental < 2; ++incremental) {
        size_t i, chunk = incremental ? 17 : sizes[f];
        decode(&full, data[f], sizes[f], modes[m], 0, crop, scale,
               filtering, chunk);
        decode(&luma, data[f], sizes[f], modes[m], 1, crop, scale,
               filtering, chunk);
        assert(full.width == luma.width && full.height == luma.height);
        if (WebPIsRGBMode(modes[m])) {
            assert(memcmp(full.pixels, luma.pixels, sizeof(full.pixels)) == 0);
        } else {
            assert(memcmp(full.pixels, luma.pixels, PLANE) == 0);
            assert(memcmp(full.pixels + 3 * PLANE,
                          luma.pixels + 3 * PLANE, PLANE) == 0);
            for (i = PLANE; i < 3 * PLANE; ++i)
                assert(luma.pixels[i] == SENTINEL);
        }
        ++cases;
    }
    printf("luma-only: %u paired decoder cases PASS\n", cases);
    return 0;
}
