// Copyright 2014 Google Inc. All Rights Reserved.
//
// Use of this source code is governed by a BSD-style license
// that can be found in the COPYING file in the root of the source
// tree. An additional intellectual property rights grant can be found
// in the file PATENTS. All contributing project authors may
// be found in the AUTHORS file in the root of the source tree.
// -----------------------------------------------------------------------------
//
// Rescaling functions
//
// Author: Skal (pascal.massimino@gmail.com)

#include <assert.h>
#include <stddef.h>

#include "zim_overlay.h"

#include "src/dsp/cpu.h"
#include "src/dsp/dsp.h"
#include "src/utils/rescaler_utils.h"
#include "src/webp/types.h"

//------------------------------------------------------------------------------
// Implementations of critical functions ImportRow / ExportRow

#define ROUNDER (WEBP_RESCALER_ONE >> 1)
#if defined(__c33__)
// GCC has no 32x32->64 multiply pattern for the C33 and calls the generic
// 64-bit __muldi3 for every pixel.  The core's mltu.w leaves the full product
// in ALR/AHR, which is exactly the high word (plus rounding) needed here.
static WEBP_INLINE uint32_t C33MultHigh(uint32_t x, uint32_t y,
                                        uint32_t* const low) {
  uint32_t lo, hi;
  __asm__ ("mltu.w\t%2, %3\n\tld.w\t%0, %%alr\n\tld.w\t%1, %%ahr"
           : "=r"(lo), "=r"(hi) : "r"(x), "r"(y));
  *low = lo;
  return hi;
}
static WEBP_INLINE uint32_t C33MultFix(uint32_t x, uint32_t y) {
  uint32_t lo;
  const uint32_t hi = C33MultHigh(x, y, &lo);
  return hi + (lo >> (WEBP_RESCALER_RFIX - 1));
}
static WEBP_INLINE uint32_t C33MultFixFloor(uint32_t x, uint32_t y) {
  uint32_t lo;
  return C33MultHigh(x, y, &lo);
}
#define MULT_FIX(x, y) C33MultFix((uint32_t)(x), (uint32_t)(y))
#define MULT_FIX_FLOOR(x, y) C33MultFixFloor((uint32_t)(x), (uint32_t)(y))
#else
#define MULT_FIX(x, y) (((uint64_t)(x) * (y) + ROUNDER) >> WEBP_RESCALER_RFIX)
#define MULT_FIX_FLOOR(x, y) (((uint64_t)(x) * (y)) >> WEBP_RESCALER_RFIX)
#endif

//------------------------------------------------------------------------------
// Row import
WEBP_UBSAN_IGNORE_UNSIGNED_OVERFLOW void ZIM_OVERLAY_SECTION("ovlwebp")
WebPRescalerImportRowExpand_C(WebPRescaler* WEBP_RESTRICT const wrk,
                              const uint8_t* WEBP_RESTRICT src) {
  const int x_stride = wrk->num_channels;
  const int x_out_max = wrk->dst_width * wrk->num_channels;
  // WikiReader: the stores through frow may alias *wrk, so without these
  // locals every pixel reloads the fields from SDRAM.
  rescaler_t* const frow = wrk->frow;
  const int x_add = wrk->x_add;
  const int x_sub = wrk->x_sub;
  int channel;
  assert(!WebPRescalerInputDone(wrk));
  assert(wrk->x_expand);
  for (channel = 0; channel < x_stride; ++channel) {
    int x_in = channel;
    int x_out = channel;
    // simple bilinear interpolation
    int accum = x_add;
    rescaler_t left = (rescaler_t)src[x_in];
    rescaler_t right =
        (wrk->src_width > 1) ? (rescaler_t)src[x_in + x_stride] : left;
    x_in += x_stride;
    while (1) {
      frow[WEBP_RESCALER_ROW_STEP * x_out] = right * x_add + (left - right) * accum;
      x_out += x_stride;
      if (x_out >= x_out_max) break;
      accum -= x_sub;
      if (accum < 0) {
        left = right;
        x_in += x_stride;
        assert(x_in < wrk->src_width * x_stride);
        right = (rescaler_t)src[x_in];
        accum += x_add;
      }
    }
    assert(wrk->x_sub == 0 /* <- special case for src_width=1 */ || accum == 0);
  }
}

void ZIM_OVERLAY_SECTION("ovlwebp")
WebPRescalerImportRowShrink_C(WebPRescaler* WEBP_RESTRICT const wrk,
                                   const uint8_t* WEBP_RESTRICT src) {
  const int x_stride = wrk->num_channels;
  const int x_out_max = wrk->dst_width * wrk->num_channels;
  // WikiReader: as above, keep the fields out of the pixel loop.
  rescaler_t* const frow = wrk->frow;
  const int x_add = wrk->x_add;
  const int x_sub = wrk->x_sub;
  const uint32_t fx_scale = wrk->fx_scale;
  int channel;
  assert(!WebPRescalerInputDone(wrk));
  assert(!wrk->x_expand);
  for (channel = 0; channel < x_stride; ++channel) {
    int x_in = channel;
    int x_out = channel;
    uint32_t sum = 0;
    int accum = 0;
    while (x_out < x_out_max) {
      uint32_t base = 0;
      accum += x_add;
      while (accum > 0) {
        accum -= x_sub;
        assert(x_in < wrk->src_width * x_stride);
        base = src[x_in];
        sum += base;
        x_in += x_stride;
      }
      {  // Emit next horizontal pixel.
        const rescaler_t frac = base * (-accum);
        frow[WEBP_RESCALER_ROW_STEP * x_out] = sum * x_sub - frac;
        // fresh fractional start for next pixel
        sum = (int)MULT_FIX(frac, fx_scale);
      }
      x_out += x_stride;
    }
    assert(accum == 0);
  }
}

//------------------------------------------------------------------------------
// Row export

void ZIM_OVERLAY_SECTION("ovlwebp")
WebPRescalerExportRowExpand_C(WebPRescaler* const wrk) {
  int x_out;
  uint8_t* const dst = wrk->dst;
  rescaler_t* const irow = wrk->irow;
  const int x_out_max = wrk->dst_width * wrk->num_channels;
  const rescaler_t* const frow = wrk->frow;
  const uint32_t fy_scale = wrk->fy_scale;  // WikiReader: out of the loop
  assert(!WebPRescalerOutputDone(wrk));
  assert(wrk->y_accum <= 0);
  assert(wrk->y_expand);
  assert(wrk->y_sub != 0);
  if (wrk->y_accum == 0) {
    for (x_out = 0; x_out < x_out_max; ++x_out) {
      const uint32_t J = frow[WEBP_RESCALER_ROW_STEP * x_out];
      const int v = (int)MULT_FIX(J, fy_scale);
      dst[x_out] = (v > 255) ? 255u : (uint8_t)v;
    }
  } else {
    const uint32_t B = WEBP_RESCALER_FRAC(-wrk->y_accum, wrk->y_sub);
    const uint32_t A = (uint32_t)(WEBP_RESCALER_ONE - B);
    for (x_out = 0; x_out < x_out_max; ++x_out) {
      const uint64_t I = (uint64_t)A * frow[WEBP_RESCALER_ROW_STEP * x_out] +
                         (uint64_t)B * irow[WEBP_RESCALER_ROW_STEP * x_out];
      const uint32_t J = (uint32_t)((I + ROUNDER) >> WEBP_RESCALER_RFIX);
      const int v = (int)MULT_FIX(J, fy_scale);
      dst[x_out] = (v > 255) ? 255u : (uint8_t)v;
    }
  }
}

#define EXPORT_ROW_SHRINK(IROW_TYPE, IROW, ISTEP)                            \
  do {                                                                       \
    int x_out;                                                               \
    uint8_t* const dst = wrk->dst;                                           \
    IROW_TYPE* const irow = wrk->IROW;                                       \
    const int x_out_max = wrk->dst_width * wrk->num_channels;                \
    const rescaler_t* const frow = wrk->frow;                                \
    const uint32_t yscale = wrk->fy_scale * (-wrk->y_accum);                 \
    const uint32_t fxy_scale = wrk->fxy_scale; /* WikiReader: hoisted */     \
    assert(!WebPRescalerOutputDone(wrk));                                    \
    assert(wrk->y_accum <= 0);                                               \
    assert(!wrk->y_expand);                                                  \
    if (yscale) {                                                            \
      for (x_out = 0; x_out < x_out_max; ++x_out) {                          \
        const uint32_t frac = (uint32_t)MULT_FIX_FLOOR(frow[WEBP_RESCALER_ROW_STEP * x_out],     \
                                                          yscale);           \
        const int v = (int)MULT_FIX(irow[ISTEP * x_out] - frac, fxy_scale);  \
        dst[x_out] = (v > 255) ? 255u : (uint8_t)v;                          \
        irow[ISTEP * x_out] = frac; /* new fractional start */               \
      }                                                                      \
    } else {                                                                 \
      for (x_out = 0; x_out < x_out_max; ++x_out) {                          \
        const int v = (int)MULT_FIX(irow[ISTEP * x_out], fxy_scale);         \
        dst[x_out] = (v > 255) ? 255u : (uint8_t)v;                          \
        irow[ISTEP * x_out] = 0;                                             \
      }                                                                      \
    }                                                                        \
  } while (0)

void ZIM_OVERLAY_SECTION("ovlwebp")
WebPRescalerExportRowShrink_C(WebPRescaler* const wrk) {
  EXPORT_ROW_SHRINK(rescaler_t, irow, WEBP_RESCALER_ROW_STEP);
}

static void RescalerExportRowShrink64_C(WebPRescaler* const wrk) {
  EXPORT_ROW_SHRINK(rescaler_accum_t, irow64, 1);
}

#undef EXPORT_ROW_SHRINK

#undef MULT_FIX_FLOOR
#undef MULT_FIX
#undef ROUNDER

//------------------------------------------------------------------------------
// Main entry calls

void ZIM_OVERLAY_SECTION("ovlwebp")
WebPRescalerImportRow(WebPRescaler* WEBP_RESTRICT const wrk,
                           const uint8_t* WEBP_RESTRICT src) {
  assert(!WebPRescalerInputDone(wrk));
  if (!wrk->x_expand) {
    WebPRescalerImportRowShrink(wrk, src);
  } else {
    WebPRescalerImportRowExpand(wrk, src);
  }
}

void ZIM_OVERLAY_SECTION("ovlwebp")
WebPRescalerExportRow(WebPRescaler* const wrk) {
  if (wrk->y_accum <= 0) {
    assert(!WebPRescalerOutputDone(wrk));
    if (wrk->y_expand) {
      WebPRescalerExportRowExpand(wrk);
    } else if (wrk->fxy_scale) {
      if (wrk->use_64bit) {
        RescalerExportRowShrink64_C(wrk);
      } else {
        WebPRescalerExportRowShrink(wrk);
      }
    } else {  // special case
      int i;
      if (wrk->use_64bit) {
        for (i = 0; i < wrk->num_channels * wrk->dst_width; ++i) {
          wrk->dst[i] = (uint8_t)wrk->irow64[i];
          wrk->irow64[i] = 0;
        }
      } else {
        assert(wrk->src_height == wrk->dst_height && wrk->x_add == 1);
        assert(wrk->src_width == 1 && wrk->dst_width <= 2);
        for (i = 0; i < wrk->num_channels * wrk->dst_width; ++i) {
          wrk->dst[i] = (uint8_t)wrk->irow[WEBP_RESCALER_ROW_STEP * i];
          wrk->irow[WEBP_RESCALER_ROW_STEP * i] = 0;
        }
      }
    }
    wrk->y_accum += wrk->y_add;
    wrk->dst += wrk->dst_stride;
    ++wrk->dst_y;
  }
}

//------------------------------------------------------------------------------

WebPRescalerImportRowFunc WebPRescalerImportRowExpand;
WebPRescalerImportRowFunc WebPRescalerImportRowShrink;

WebPRescalerExportRowFunc WebPRescalerExportRowExpand;
WebPRescalerExportRowFunc WebPRescalerExportRowShrink;

extern VP8CPUInfo VP8GetCPUInfo;
extern void WebPRescalerDspInitSSE2(void);
extern void WebPRescalerDspInitMIPS32(void);
extern void WebPRescalerDspInitMIPSdspR2(void);
extern void WebPRescalerDspInitMSA(void);
extern void WebPRescalerDspInitNEON(void);

WEBP_DSP_INIT_FUNC(WebPRescalerDspInit) {
#if !defined(WEBP_REDUCE_SIZE)
#if !WEBP_NEON_OMIT_C_CODE
  WebPRescalerExportRowExpand = WebPRescalerExportRowExpand_C;
  WebPRescalerExportRowShrink = WebPRescalerExportRowShrink_C;
#endif

  WebPRescalerImportRowExpand = WebPRescalerImportRowExpand_C;
  WebPRescalerImportRowShrink = WebPRescalerImportRowShrink_C;

  if (VP8GetCPUInfo != NULL) {
#if defined(WEBP_HAVE_SSE2)
    if (VP8GetCPUInfo(kSSE2)) {
      WebPRescalerDspInitSSE2();
    }
#endif
#if defined(WEBP_USE_MIPS32)
    if (VP8GetCPUInfo(kMIPS32)) {
      WebPRescalerDspInitMIPS32();
    }
#endif
#if defined(WEBP_USE_MIPS_DSP_R2)
    if (VP8GetCPUInfo(kMIPSdspR2)) {
      WebPRescalerDspInitMIPSdspR2();
    }
#endif
#if defined(WEBP_USE_MSA)
    if (VP8GetCPUInfo(kMSA)) {
      WebPRescalerDspInitMSA();
    }
#endif
  }

#if defined(WEBP_HAVE_NEON)
  if (WEBP_NEON_OMIT_C_CODE ||
      (VP8GetCPUInfo != NULL && VP8GetCPUInfo(kNEON))) {
    WebPRescalerDspInitNEON();
  }
#endif

  assert(WebPRescalerExportRowExpand != NULL);
  assert(WebPRescalerExportRowShrink != NULL);
  assert(WebPRescalerImportRowExpand != NULL);
  assert(WebPRescalerImportRowShrink != NULL);
#endif  // WEBP_REDUCE_SIZE
}
