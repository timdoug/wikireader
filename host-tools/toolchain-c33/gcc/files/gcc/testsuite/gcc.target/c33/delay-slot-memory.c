/* { dg-do compile } */
/* { dg-options "-O2 -fdelayed-branch" } */

extern void sink (int);

int
load_before_branch (const int *p, int x)
{
  int value = *p;
  if (x)
    return value;
  return value + 1;
}

void
store_before_branch (int *p, int x)
{
  *p = x;
  if (x)
    sink (x);
}

/* PE delayed slots may not access memory.  */
/* { dg-final { scan-assembler-not {jrne[.]d[^\n]*\n[ \t]+ld[.]w[^\n]*[[]%r[0-9]+[]]} } } */
