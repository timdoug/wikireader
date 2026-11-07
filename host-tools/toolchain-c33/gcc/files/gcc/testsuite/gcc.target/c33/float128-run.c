/* { dg-do run } */
/* { dg-options "-O2" } */

/* _Float128 in software (libgcc's soft-fp): arithmetic, conversions to and
   from double and long long, and comparison, against bit patterns computed
   exactly elsewhere.  */

typedef unsigned long long u64;

static void
bits (_Float128 x, u64 *hi, u64 *lo)
{
  union { _Float128 f; u64 w[2]; } u = { .f = x };
  *lo = u.w[0];
  *hi = u.w[1];
}

static int
is (_Float128 x, u64 hi, u64 lo)
{
  u64 h, l;
  bits (x, &h, &l);
  return h == hi && l == lo;
}

/* volatile, so the arithmetic is done at run time by libgcc.  */
volatile _Float128 one = 1, three = 3, one_and_half = 1.5, quarter = 0.25;
volatile double third_d = 1.0 / 3.0;
volatile long long ten = 10;

int
main (void)
{
  if (!is (one_and_half + quarter, 0x3fffc00000000000ULL, 0))		/* 1.75 */
    __builtin_abort ();
  if (!is (one_and_half * 7, 0x4002500000000000ULL, 0))		/* 10.5 */
    __builtin_abort ();
  if (!is (one / three, 0x3ffd555555555555ULL, 0x5555555555555555ULL))
    __builtin_abort ();
  if ((double) (one / three) != third_d)		/* truncation to double */
    __builtin_abort ();
  if ((long long) (one_and_half * 7) != ten)		/* to an integer */
    __builtin_abort ();
  if ((_Float128) ten != one_and_half * 7 - 0.5)	/* from an integer */
    __builtin_abort ();
  if (!(one / three < one_and_half) || one / three == (_Float128) third_d)
    __builtin_abort ();
  return 0;
}
