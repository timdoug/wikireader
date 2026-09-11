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

unsigned wr_divmod_unsigned(unsigned n, unsigned d, int remainder)
{
    unsigned bit = 1, quotient;
    if (!d || n < d) return remainder ? n : 0;
    /* Align d with n without overflow. Each test also skips a block of
       leading zero quotient bits; no generic division is used here. */
    if ((n >> 16) >= d) { d <<= 16; bit <<= 16; }
    if ((n >> 8) >= d) { d <<= 8; bit <<= 8; }
    if ((n >> 4) >= d) { d <<= 4; bit <<= 4; }
    if ((n >> 2) >= d) { d <<= 2; bit <<= 2; }
    if ((n >> 1) >= d) { d <<= 1; bit <<= 1; }
    quotient = bit;
    n -= d;
    while ((bit >>= 1)) {
        d >>= 1;
        if (n >= d) { n -= d; quotient |= bit; }
    }
    return remainder ? n : quotient;
}

#ifdef WR_C33
/* libgcc's signed/unsigned / and % entry points call this ABI. Keep the
   entry in SDRAM for their short calls, and run the actual divide in A0. */
unsigned __udivmodsi4(unsigned n, unsigned d, int remainder)
{
    unsigned (*volatile divide)(unsigned, unsigned, int) = wr_divmod_unsigned;
    return divide(n, d, remainder);
}
#endif
