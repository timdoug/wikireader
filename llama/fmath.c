/* expf and rsqrtf without libm, GPL-3.0-or-later. */

#include <stdint.h>
#include <string.h>

#include "fmath.h"

static float from_bits(uint32_t b)
{
	float f;
	/* Not a union: -fno-strict-aliasing is not set for this file and
	   memcpy is what the compiler folds into a plain move anyway. */
	memcpy(&f, &b, sizeof f);
	return f;
}

static uint32_t to_bits(float f)
{
	uint32_t b;
	memcpy(&b, &f, sizeof b);
	return b;
}

/* exp(x) as 2^(x * log2 e), splitting the exponent off into the float's
   bit pattern so the only real work is a polynomial on a fraction in
   [0, 1).  The alternative is a range reduction with a division, and
   division is software here too.
 *
 * The polynomial is the degree-4 minimax fit to 2^f on [0, 1); it is worth
 * about 1e-6 relative, which is far below the noise an int8 weight already
 * puts into every activation.
 */
float wr_expf(float x)
{
	/* Guard the exponent construction rather than the result: outside
	   this range the float is 0 or inf and the shift below would be
	   undefined.  Attention scores reach neither, but a NaN that got
	   this far would otherwise index nothing sensible. */
	if (x < -87.0f)
		return 0.0f;
	if (x > 88.0f)
		return from_bits(0x7f800000u);	/* +inf */

	float y = x * 1.44269504088896f;	/* log2 e */
	/* floorf, without calling it: the cast truncates toward zero. */
	int32_t k = (int32_t)y;
	if (y < 0.0f && (float)k != y)
		k -= 1;
	float f = y - (float)k;

	float p = 0.0136779459f;
	p = p * f + 0.0517692046f;
	p = p * f + 0.2413886183f;
	p = p * f + 0.6930324459f;
	p = p * f + 0.9999999703f;

	/* 2^k by adding k to the exponent field.  k is in [-126, 127] after
	   the guards above, so the bias cannot overflow. */
	return p * from_bits((uint32_t)(k + 127) << 23);
}

/* 1/sqrt(x) by the usual exponent halving plus two Newton steps.  rmsnorm
   is the only caller and runs 2 * n_layers + 1 times a token, so accuracy
   matters more than the third step would cost.
 */
float wr_rsqrtf(float x)
{
	float half = x * 0.5f;
	float y = from_bits(0x5f3759dfu - (to_bits(x) >> 1));
	y = y * (1.5f - half * y * y);
	y = y * (1.5f - half * y * y);
	return y;
}
