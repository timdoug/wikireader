/* { dg-do compile } */
/* { dg-options "-O2 -mc33pe -fomit-frame-pointer" } */

void singleton (void)
{
  __asm__ volatile ("ld.w %%r3,7" : : : "r3");
}

void gaps (void)
{
  __asm__ volatile ("ld.w %%r0,7\n\tld.w %%r2,7" : : : "r0", "r2");
}

void prefix_and_gap (void)
{
  __asm__ volatile ("ld.w %%r0,7\n\tld.w %%r1,7\n\tld.w %%r3,7"
                    : : : "r0", "r1", "r3");
}

void dense (void)
{
  __asm__ volatile ("" : : : "r0", "r1", "r2", "r3");
}

/* One contiguous block covers every register up to the highest used one. */
/* { dg-final { scan-assembler-times "pushn\t%r2" 1 } } */
/* { dg-final { scan-assembler-times "popn\t%r2" 1 } } */
/* { dg-final { scan-assembler-times "pushn\t%r3" 3 } } */
/* { dg-final { scan-assembler-times "popn\t%r3" 3 } } */
