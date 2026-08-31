/* { dg-do compile } */
/* { dg-options "-mcore=c33pe -O2" } */
/* { dg-skip-if "code quality test" { *-*-* } { "-O0" } { "" } } */

int
select_bit_2 (const unsigned char *p, int set, int clear)
{
  return (*p & 4) ? set : clear;
}

int
select_clear_bit_7 (const unsigned char *p, int clear, int set)
{
  return (*p & 128) == 0 ? clear : set;
}

/* { dg-final { scan-assembler-times "btst\\t" 2 } } */
/* { dg-final { scan-assembler-not "xand" } } */
