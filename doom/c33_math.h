/* Exact fixed-point arithmetic for WikiReader, GPL-3.0-or-later. */
#ifndef WR_C33_MATH_H
#define WR_C33_MATH_H

static inline int wr_fixed_mul(int a, int b)
{
#ifdef WR_C33
    unsigned lo, hi;
    /* Signed 32 x 32 -> AHR:ALR. The generic compiler widening multiply
       otherwise calls __muldi3; keep bits 16..47 just as Doom does. */
    __asm__("mlt.w\t%2,%3\n\tld.w\t%0,%%alr\n\tld.w\t%1,%%ahr"
            : "=r"(lo), "=r"(hi) : "r"(a), "r"(b) : "alr", "ahr");
    return (int)((lo >> 16) | (hi << 16));
#else
    return (int)(((long long)a * b) >> 16);
#endif
}

#ifdef WR_C33
unsigned wr_reciprocal_unsigned(unsigned d)
    __attribute__((section(".fastcode"), noinline));
#else
unsigned wr_reciprocal_unsigned(unsigned d);
#endif

static inline unsigned wr_reciprocal(unsigned d)
{
#ifdef WR_C33
    /* A0 RAM is outside SDRAM's relative-call range. */
    unsigned (*volatile divide)(unsigned) = wr_reciprocal_unsigned;
    return divide(d);
#else
    return wr_reciprocal_unsigned(d);
#endif
}
#endif
