/* Integer replacements for the float math the forward pass needs.
 * GPL-3.0-or-later.
 *
 * There is no libm here and no FPU under it, so these are not an
 * optimization -- they are the only way the arithmetic happens at all.
 *
 * Everything works in one of two fixed formats:
 *   Q12  a value scaled by 4096, used where a real magnitude is needed
 *        (the argument to exp, the argument to a sigmoid);
 *   int8 with a shared exponent, used for anything a matmul touches --
 *        value[i] = q[i] * 2^-e.
 */

#ifndef WR_LLAMA_FIXED_H
#define WR_LLAMA_FIXED_H

#include <stdint.h>

#define Q12_ONE 4096

/* Shift right by `s`, or left if `s` is negative, saturating rather than
   invoking undefined behaviour at the extremes.  Inline: the attention
   path calls it once per cached position. */
static inline int32_t wr_sshift(int32_t v, int s)
{
	if (s > 0)
		return s >= 31 ? (v < 0 ? -1 : 0) : (v >> s);
	if (s < 0)
		return -s >= 31 ? 0 : (int32_t)((uint32_t)v << -s);
	return v;
}

/* exp(x) for x <= 0, argument and result both Q12.  Returns 0 once the
   true value is below half a Q12 step.  The only callers are the attention
   softmax and the SwiGLU sigmoid, which both want exp of something
   non-positive. */
int32_t wr_exp_q12(int32_t x);

/* floor(sqrt(x)).  RMSNorm is the only caller, 2 * n_layers + 1 times a
   token, so this is a plain bit-by-bit root rather than a table. */
uint32_t wr_isqrt64(uint64_t x);

/* Position of the highest set bit, or -1 for zero.  The C33 PE core has no
   SCAN0/SCAN1 -- they are among the instructions the PE variant drops --
   and calling libgcc's __clzsi2 showed up at 0.6% of the profile. */
int wr_ilog2(uint32_t x);

#endif
