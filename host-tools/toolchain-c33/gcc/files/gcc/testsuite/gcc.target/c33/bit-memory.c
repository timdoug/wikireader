/* { dg-do compile } */
/* { dg-options "-mcore=c33pe -O2" } */
/* { dg-skip-if "code quality test" { *-*-* } { "-O0" } { "" } } */

void
set_bit (unsigned char *p)
{
  *p |= 4;
}

void
clear_bit (unsigned char *p)
{
  *p &= (unsigned char) ~4;
}

void
toggle_bit (unsigned char *p)
{
  *p ^= 4;
}

/* { dg-final { scan-assembler-times "bset\\t" 1 } } */
/* { dg-final { scan-assembler-times "bclr\\t" 1 } } */
/* { dg-final { scan-assembler-times "bnot\\t" 1 } } */
/* { dg-final { scan-assembler-not "xoor" } } */
/* { dg-final { scan-assembler-not "xand" } } */
/* { dg-final { scan-assembler-not "xxor" } } */
