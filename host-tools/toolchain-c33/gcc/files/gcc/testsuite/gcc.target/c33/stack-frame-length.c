/* { dg-do compile } */
/* { dg-options "-mcore=c33pe -O2 -dp" } */
/* { dg-skip-if "code quality test" { *-*-* } { "-O0" } { "" } } */

extern void use (volatile unsigned char *);

void
frame_4096 (void)
{
  volatile unsigned char data[4096];
  use (data);
}

void
frame_524288 (void)
{
  volatile unsigned char data[524288];
  use (data);
}

/* The two register copies add four bytes around the narrowed arithmetic.  */
/* { dg-final { scan-assembler-times "l=8.*add_sp_big" 1 } } */
/* { dg-final { scan-assembler-times "l=10.*add_sp_big" 1 } } */
