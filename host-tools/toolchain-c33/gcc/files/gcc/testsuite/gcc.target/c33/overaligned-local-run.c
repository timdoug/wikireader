/* { dg-do run } */
/* { dg-options "-O2" } */

/* A local aligned beyond the 4 bytes %sp keeps is laid out at an aligned
   offset below the frame pointer, so the frame pointer has to be that
   aligned: in a function that calls, with outgoing arguments, and in a
   leaf.  The address is read through an asm, which GCC cannot fold to the
   alignment it believes in.  */

struct __attribute__((aligned(16))) tagme { unsigned long long a, b; };

static unsigned long
low_bits (void *p)
{
  unsigned long v;
  __asm__ ("ld.w %0,%1" : "=r" (v) : "r" (p));
  return v & 15;
}

__attribute__((noinline, noclone)) static void
take (volatile void *p, int a, int b, int c, int d, int e, int f)
{
  (void) p; (void) a; (void) b; (void) c; (void) d; (void) e; (void) f;
}

__attribute__((noinline, noclone)) static unsigned long
caller (void)
{
  struct tagme t = { 1000, 0 };
  take (&t, 1, 2, 3, 4, 5, 6);		/* two words of outgoing arguments */
  return low_bits (&t);
}

__attribute__((noinline, noclone)) static unsigned long
leaf (int x)
{
  volatile struct tagme t = { (unsigned long long) x, 0 };
  return low_bits ((void *) &t) | (t.a != (unsigned long long) x);
}

int
main (void)
{
  int i;

  for (i = 0; i < 4; i++)
    {
      /* Enter at each of the four word offsets a misplaced frame could
         land on.  */
      volatile char pad[4 * i + 1];
      pad[0] = 0;
      if (caller () != 0 || leaf (i) != 0)
	__builtin_abort ();
    }
  return 0;
}
