/* Integer math for the forward pass, GPL-3.0-or-later. */

#include "fixed.h"

int wr_ilog2(uint32_t x)
{
	int n = -1;

	/* Binary search rather than a loop: the loop version costs a branch
	   per bit and this runs inside every vector normalization. */
	if (x & 0xffff0000u) { n += 16; x >>= 16; }
	if (x & 0x0000ff00u) { n += 8;  x >>= 8; }
	if (x & 0x000000f0u) { n += 4;  x >>= 4; }
	if (x & 0x0000000cu) { n += 2;  x >>= 2; }
	if (x & 0x00000002u) { n += 1;  x >>= 1; }
	if (x & 0x00000001u) { n += 1; }
	return n;
}

uint32_t wr_isqrt64(uint64_t x)
{
	uint64_t rem = 0, root = 0;
	int i;

	/* Bit by bit from the top, two bits of radicand per bit of root.
	   Thirty-two iterations, no division, nothing wider than the 64-bit
	   adds gcc already inlines on this part. */
	for (i = 0; i < 32; i++) {
		root <<= 1;
		rem = (rem << 2) | (x >> 62);
		x <<= 2;
		if (rem > root) {
			rem -= root | 1;
			root += 2;
		}
	}
	return (uint32_t)(root >> 1);
}

int32_t wr_exp_q12(int32_t x)
{
	int32_t y, f, p;
	int k;

	if (x >= 0)
		return Q12_ONE;
	/* exp(-12) is 6.1e-6, which is a fortieth of a Q12 step. */
	if (x <= -12 * Q12_ONE)
		return 0;

	/* y = x * log2(e), Q12.  |x| < 49152 and 5909 is log2(e) in Q12, so
	   the product is under 2^29 and needs no widening. */
	y = (x * 5909) >> 12;
	k = y >> 12;			/* arithmetic shift floors */
	f = y - (k << 12);		/* f in [0, 4096) */

	/* 2^f on [0,1) by Horner, Q12 throughout.  p stays under 2^13 and f
	   under 2^12, so each product is under 2^25.  The fifth-order term
	   is worth keeping: without it the series is short by 0.00133 at
	   f = 1, which is five Q12 steps and the whole error budget.  Each
	   step rounds rather than truncating, because five truncations
	   compound into more error than the dropped term did. */
	p = 5;					/* 0.0013333 */
	p = ((p * f + 2048) >> 12) + 39;	/* 0.0096181 */
	p = ((p * f + 2048) >> 12) + 227;	/* 0.0555041 */
	p = ((p * f + 2048) >> 12) + 984;	/* 0.2402265 */
	p = ((p * f + 2048) >> 12) + 2839;	/* 0.6931472 */
	p = ((p * f + 2048) >> 12) + 4096;	/* 1 */

	/* k is in [-18, 0] after the guard above. */
	return p >> -k;
}
