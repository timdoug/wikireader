/* Exact unsigned arithmetic for WikiReader, GPL-3.0-or-later. */
#include "c33_math.h"

unsigned wr_reciprocal_unsigned(unsigned d)
{
    unsigned bit = 1, quotient, remainder;
    /* Valid rendering scales are nonzero; keep malformed input bounded. */
    if (!d) return 0;
    /* Normalize in five tests instead of a bit-at-a-time software loop. */
    if (d <= 0xffffu) { d <<= 16; bit <<= 16; }
    if (d <= 0xffffffu) { d <<= 8; bit <<= 8; }
    if (d <= 0xfffffffu) { d <<= 4; bit <<= 4; }
    if (d <= 0x3fffffffu) { d <<= 2; bit <<= 2; }
    if (d <= 0x7fffffffu) { d <<= 1; bit <<= 1; }
    /* The first subtraction from UINT_MAX always succeeds. */
    quotient = bit;
    remainder = ~d;
    while ((bit >>= 1)) {
        d >>= 1;
        if (remainder >= d) { remainder -= d; quotient |= bit; }
    }
    return quotient;
}

/* The general n / d lives in libgcc now: this same algorithm is
   __udivmodsi4 there (host-tools/toolchain-c33/gcc/files/libgcc/config/c33),
   so every / and % in the program gets it and Doom has nothing to override.
   Only the fixed-numerator reciprocal above is worth keeping here. */
