/* { dg-do compile } */
/* { dg-options "-mcore=c33pe -O2" } */
/* { dg-skip-if "code quality test" { *-*-* } { "-O0" } { "" } } */

unsigned char absolute_byte;

void set_1 (unsigned char *p)       { p[1] |= 4; }
void clear_8191 (unsigned char *p)  { p[8191] &= (unsigned char) ~4; }
void toggle_8192 (unsigned char *p) { p[8192] ^= 4; }
int test_1 (const unsigned char *p, int set, int clear)
{
  return (p[1] & 4) ? set : clear;
}
void set_absolute (void)            { absolute_byte |= 4; }

/* A nonzero general-base displacement takes xbit; 8191 is the last one-ext
   address and 8192 is the first two-ext address.  */
/* { dg-final { scan-assembler-times "xbset\\t" 1 } } */
/* { dg-final { scan-assembler-times "xbclr\\t" 1 } } */
/* { dg-final { scan-assembler-times "xbnot\\t" 1 } } */
/* { dg-final { scan-assembler-times "xbtst\\t" 1 } } */
/* { dg-final { scan-assembler-not "xoor" } } */
/* { dg-final { scan-assembler-not "xand" } } */
/* { dg-final { scan-assembler-not "xxor" } } */
