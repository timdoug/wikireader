/* { dg-do run } */
/* { dg-options "-O2" } */

static volatile unsigned calls;
static volatile int value = 2;
static volatile unsigned count = 1;

__attribute__((noinline))
static void
called (void)
{
  ++calls;
}

__attribute__((noinline))
static void
positive_shift (int value, unsigned count)
{
  int shifted = value >> count;

  if (shifted > 0)
    called ();
}

int
main (void)
{
  calls = 0;

  /* Leave V set before the call.  Shifts update N and Z but preserve V, so
     a signed branch may not consume their flags as though they came from a
     compare.  0x80000000 - 1 overflows to a nonnegative result.  */
  __asm__ volatile ("xld.w %%r4,-2147483648\n\t"
		    "xld.w %%r5,1\n\t"
		    "cmp %%r4,%%r5"
		    : : : "r4", "r5", "cc");
  positive_shift (value, count);

  if (calls != 1)
    __builtin_abort ();
  return 0;
}
