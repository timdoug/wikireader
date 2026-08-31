/* { dg-do compile } */
/* { dg-options "-O2 -ftrampolines" } */

typedef int (*callback) (int);

extern int invoke (callback, int);

int
outer (int x)
{
  int nested (int y) { return x + y; }
  return invoke (nested, 3);
}

/* The PE manual defines a delayed ld.w from PC for this purpose.  */
/* { dg-final { scan-assembler {jp[.]d[ \t]+[.][+]4\n[ \t]+ld[.]w[ \t]+%r12,%pc} } } */
/* { dg-final { scan-assembler-not {call[ \t]+[.][+]2} } } */
