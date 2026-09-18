/* Differential test: real C33 instructions versus host 64-bit arithmetic. */
#include "dt.h"
#include "../c33_math.h"

#ifndef DT_HOST
extern unsigned long __udivmodsi4(unsigned long num, unsigned long den,
                                  int modwanted);
#endif

static unsigned reciprocal(unsigned d)
{
#ifdef DT_HOST
    return d ? ~0u / d : 0;
#else
    return wr_reciprocal(d);
#endif
}

static void pair(int a, int b)
{
    emit((unsigned)wr_fixed_mul(a, b));
    /* Interleaved multiplies also exercise the accumulator clobbers. */
    emit((unsigned)wr_fixed_mul(a, b) ^ (unsigned)wr_fixed_mul(b, 17) ^
         (unsigned)wr_fixed_mul(a, -71341) ^ ((unsigned)a * 23u));
    emit(reciprocal((unsigned)b));
}

static void division(unsigned n, unsigned d)
{
    /* The target's ordinary operators exercise libgcc's entry points and
       our SDRAM-to-A0 bridge, including signed quotient/remainder rules. */
    if (d) {
        emit(n / d); emit(n % d);
        if (!((int)n == (-2147483647-1) && (int)d == -1)) {
            emit((unsigned)((int)n / (int)d));
            emit((unsigned)((int)n % (int)d));
        }
    }
    /* libgcc's divide entered directly, which is the only way to reach the
       zero divisor: it answers a zero quotient and the dividend, as the
       generic routine this replaced did. */
#ifdef DT_HOST
    emit(d ? n / d : 0); emit(d ? n % d : n);
#else
    emit(__udivmodsi4(n, d, 0)); emit(__udivmodsi4(n, d, 1));
#endif
}

int main(void)
{
    static const int edges[] = {0, 1, -1, 65535, -65535, 65536, -65536,
        65537, -65537, 2147483647, (-2147483647-1), 0x12345678, -0x12345678};
    unsigned i, j, random = 12345;
    for (i = 0; i < sizeof(edges)/sizeof(*edges); ++i)
        for (j = 0; j < sizeof(edges)/sizeof(*edges); ++j) {
            pair(edges[i], edges[j]);
            division((unsigned)edges[i], (unsigned)edges[j]);
        }
    for (i = 1; i <= 65536; ++i) emit(reciprocal(i));
    for (i = 0; i < 32; ++i) {
        unsigned d = 1u << i;
        emit(reciprocal(d-1)); emit(reciprocal(d)); emit(reciprocal(d+1));
        division(~0u, d); division(~0u, d-1); division(~0u, d+1);
    }
    for (i = 0; i < 10000; ++i) {
        int a;
        random = random * 1664525u + 1013904223u; a = (int)random;
        random = random * 1664525u + 1013904223u;
        pair(a, (int)random);
        division((unsigned)a, random);
        division(random, (random >> 16) + 1);
        emit(reciprocal((random & 0x3fffffu) + 1));
    }
    emit(0xd00dcafeu);
    return 0;
}
