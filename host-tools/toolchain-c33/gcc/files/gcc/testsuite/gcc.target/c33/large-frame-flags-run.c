/* { dg-do run } */
/* { dg-options "-O2" } */

/* The general-register adjustment used for a frame beyond 4092 bytes
   clobbers flags.  A comparison must not cross that adjustment. */
__attribute__((noinline, noclone))
static int choose(int value)
{
  volatile char local[5000];
  if (value == 1)
    {
      local[0] = 11;
      return local[0];
    }
  local[0] = 22;
  return local[0];
}

int main(void)
{
  if (choose(1) != 11 || choose(0) != 22 || choose(2) != 22)
    __builtin_abort();
  return 0;
}
