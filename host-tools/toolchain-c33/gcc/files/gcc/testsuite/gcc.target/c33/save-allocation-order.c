/* { dg-do compile } */
/* { dg-options "-O2 -mc33pe -fomit-frame-pointer -fno-optimize-sibling-calls" } */

extern void consume (unsigned int);
__attribute__((optimize ("no-unroll-loops", "no-peel-loops", "no-tracer")))
void countdown (unsigned int n)
{
  while (n)
    consume (n--);
}

/* A single value live across calls should use the first saved register. */
/* { dg-final { scan-assembler "pushn\t%r0" } } */
/* { dg-final { scan-assembler-not "pushn\t%r(1|2|3)" } } */
