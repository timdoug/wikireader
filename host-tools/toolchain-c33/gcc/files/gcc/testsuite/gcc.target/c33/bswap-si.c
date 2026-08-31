/* { dg-do compile } */
/* { dg-options "-O2" } */

__attribute__((noinline))
unsigned
swap32 (unsigned x)
{
  return __builtin_bswap32 (x);
}

/* { dg-final { scan-assembler "swap\\t" } } */
/* { dg-final { scan-assembler-not "__bswapsi2" } } */
