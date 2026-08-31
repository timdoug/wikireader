/* { dg-do compile } */
/* { dg-options "-mcore=c33pe -O2" } */

unsigned short
swap16 (unsigned short value)
{
  return __builtin_bswap16 (value);
}

/* { dg-final { scan-assembler "swaph\\t" } } */
/* { dg-final { scan-assembler-not "xsll" } } */
/* { dg-final { scan-assembler-not "xsrl" } } */
