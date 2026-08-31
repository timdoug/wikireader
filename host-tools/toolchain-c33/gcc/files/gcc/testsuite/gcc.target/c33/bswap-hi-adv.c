/* { dg-do compile } */
/* { dg-options "-mcore=c33adv -O2" } */

unsigned short
swap16 (unsigned short value)
{
  return __builtin_bswap16 (value);
}

/* swaph is common to the Advanced and PE cores, but absent from STD.  */
/* { dg-final { scan-assembler "swaph\\t" } } */
/* { dg-final { scan-assembler-not "xsll" } } */
/* { dg-final { scan-assembler-not "xsrl" } } */
