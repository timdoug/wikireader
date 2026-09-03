/* { dg-do compile } */
/* { dg-options "-O2" } */

int
find_high_bit (const unsigned char *text, int length)
{
  while (length > 0)
    {
      if (text[length - 1] & 0x80)
	break;
      --length;
    }
  return length;
}
