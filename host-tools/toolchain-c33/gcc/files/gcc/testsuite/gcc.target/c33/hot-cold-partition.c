/* A branch between a function's hot and cold partitions has its target in
   another section, so it takes the 32-bit form however near the two
   partitions appear before linking.  */
/* { dg-do run } */
/* { dg-options "-O2 -freorder-blocks-and-partition -save-temps" } */
/* { dg-final { scan-assembler "\\.text\\.unlikely" } } */
/* { dg-final { scan-assembler "\\txjr\[a-z\]+(\\.d)?\\t" } } */
/* { dg-final { scan-assembler "\\txjp(\\.d)?\\t" } } */

volatile int seen;

__attribute__((noinline, cold)) static void
report (int i)
{
  seen = i;
}

__attribute__((noinline)) int
sum (const int *v, int n)
{
  int s = 0;

  for (int i = 0; i < n; i++)
    {
      if (v[i] < 0)
	{
	  report (i);
	  s -= v[i];
	}
      else
	s += v[i] * 3;
    }
  return s;
}

int
main (void)
{
  static const int v[] = { 1, 2, -4, 5 };

  return sum (v, 4) != 28 || seen != 2;
}
