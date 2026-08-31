/* { dg-do run } */
/* { dg-options "-mcore=c33pe -O2" } */

__attribute__((noinline))
static void
set_bit (unsigned char *p)
{
  *p |= 4;
}

__attribute__((noinline))
static void
clear_bit (unsigned char *p)
{
  *p &= (unsigned char) ~4;
}

__attribute__((noinline))
static void
toggle_bit (unsigned char *p)
{
  *p ^= 4;
}

int
main (void)
{
  unsigned char value = 0x51;

  set_bit (&value);
  if (value != 0x55)
    __builtin_abort ();
  clear_bit (&value);
  if (value != 0x51)
    __builtin_abort ();
  toggle_bit (&value);
  if (value != 0x55)
    __builtin_abort ();
  toggle_bit (&value);
  if (value != 0x51)
    __builtin_abort ();
  return 0;
}
