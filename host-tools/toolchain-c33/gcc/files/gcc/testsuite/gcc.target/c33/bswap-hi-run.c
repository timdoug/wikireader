/* { dg-do run } */
/* { dg-options "-mcore=c33pe -O2" } */

__attribute__((noinline))
static unsigned short
swap16 (unsigned short value)
{
  return __builtin_bswap16 (value);
}

int
main (void)
{
  if (swap16 (0x1234u) != 0x3412u)
    __builtin_abort ();
  return 0;
}
