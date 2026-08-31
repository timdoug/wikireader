/* { dg-do compile } */
/* { dg-options "-mcore=c33pe -O2" } */

unsigned char
set_stack (void)
{
  volatile unsigned char byte = 1;
  byte |= 4;
  return byte;
}

/* The C33 bit-memory encoding has no stack-pointer form.  */
/* { dg-final { scan-assembler-not "x?bset\\t\\\[%sp" } } */
