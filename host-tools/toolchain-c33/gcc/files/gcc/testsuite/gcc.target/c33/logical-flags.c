/* { dg-do compile } */
/* { dg-options "-O2" } */

extern void sink (void);

void
and_zero (unsigned left, unsigned right)
{
  if ((left & right) == 0)
    sink ();
}

void
or_zero (unsigned left, unsigned right)
{
  if ((left | right) == 0)
    sink ();
}

/* Both logical instructions already produce the Z flag consumed by jreq.  */
/* { dg-final { scan-assembler-times "jreq" 2 } } */
/* { dg-final { scan-assembler-not "cmp" } } */
