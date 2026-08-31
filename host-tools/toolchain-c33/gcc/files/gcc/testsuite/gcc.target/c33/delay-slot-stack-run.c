/* { dg-do run } */
/* { dg-options "-O2 -fdelayed-branch -foptimize-sibling-calls" } */

volatile int seen;

__attribute__((noinline)) int
sink (int x)
{
  seen = x;
  return x + 1;
}

__attribute__((noinline)) int
tail (int x)
{
  volatile int frame[8];
  frame[0] = x;
  return sink (x);
}

int
main (void)
{
  return tail (41) != 42 || seen != 41;
}
