/* { dg-do run } */
/* { dg-options "-mcore=c33pe -O2" } */

__attribute__((noinline))
static int
select_bit_2 (const unsigned char *p, int set, int clear)
{
  return (*p & 4) ? set : clear;
}

__attribute__((noinline))
static int
select_clear_bit_7 (const unsigned char *p, int clear, int set)
{
  return (*p & 128) == 0 ? clear : set;
}

int
main (void)
{
  unsigned char value = 0x04;

  if (select_bit_2 (&value, 11, 22) != 11)
    __builtin_abort ();
  if (select_clear_bit_7 (&value, 33, 44) != 33)
    __builtin_abort ();
  value = 0x80;
  if (select_bit_2 (&value, 11, 22) != 22)
    __builtin_abort ();
  if (select_clear_bit_7 (&value, 33, 44) != 44)
    __builtin_abort ();
  return 0;
}
