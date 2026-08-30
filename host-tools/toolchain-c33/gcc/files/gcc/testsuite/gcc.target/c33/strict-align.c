/* { dg-do compile } */
/* { dg-options "-O2" } */

struct __attribute__((packed)) packet {
  unsigned char tag;
  unsigned value;
};

unsigned
load_value (const struct packet *p)
{
  return p->value;
}

void
store_value (struct packet *p, unsigned value)
{
  p->value = value;
}

/* The word starts at offset one, so PE must access it bytewise.  */
/* { dg-final { scan-assembler-times "ld\\.ub" 4 } } */
/* { dg-final { scan-assembler-times "ld\\.b" 4 } } */
