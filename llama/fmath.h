/* The three float functions the forward pass needs, GPL-3.0-or-later.
 *
 * mini-libc has no math.h and the toolchain ships no libm, so these are
 * not a choice about speed -- without them the link fails.  They are
 * written to be cheap anyway, because on this part every float operation
 * inside them is a call into libgcc's soft float.
 */

#ifndef WR_LLAMA_FMATH_H
#define WR_LLAMA_FMATH_H

float wr_expf(float x);
float wr_rsqrtf(float x);

#endif
