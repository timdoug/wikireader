/* { dg-do run } */
/* { dg-options "-mcore=c33pe -O2" } */

static unsigned char bytes[9000];
static unsigned char absolute_byte;

__attribute__((noinline)) static void set_1 (unsigned char *p)       { p[1] |= 4; }
__attribute__((noinline)) static void clear_8191 (unsigned char *p)  { p[8191] &= (unsigned char) ~4; }
__attribute__((noinline)) static void toggle_8192 (unsigned char *p) { p[8192] ^= 4; }
__attribute__((noinline)) static int test_1 (const unsigned char *p, int set, int clear)
{
  return (p[1] & 4) ? set : clear;
}
__attribute__((noinline)) static void set_absolute (void)            { absolute_byte |= 4; }

int
main (void)
{
  bytes[1] = 0x51;
  bytes[8191] = 0x55;
  bytes[8192] = 0x51;

  set_1 (bytes);
  clear_8191 (bytes);
  toggle_8192 (bytes);
  set_absolute ();

  if (bytes[1] != 0x55 || test_1 (bytes, 7, 9) != 7)
    __builtin_abort ();
  if (bytes[8191] != 0x51 || bytes[8192] != 0x55)
    __builtin_abort ();
  if (absolute_byte != 4)
    __builtin_abort ();
  return 0;
}
