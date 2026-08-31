/* { dg-do run } */
/* { dg-options "-O2" } */

__attribute__((noinline))
unsigned
swap32 (unsigned x)
{
  return __builtin_bswap32 (x);
}

int
main (void)
{
  if (swap32 (0x12345678u) != 0x78563412u)
    __builtin_abort ();
  return 0;
}
