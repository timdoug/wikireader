/* { dg-do run } */
/* { dg-options "-O2 -mc33pe -fomit-frame-pointer -funwind-tables" } */

__attribute__((noinline, noclone, used)) void singleton (void)
{
  __asm__ volatile ("ld.w %%r3,7" : : : "r3");
}

__attribute__((noinline, noclone, used)) void gaps (void)
{
  __asm__ volatile ("ld.w %%r0,7\n\tld.w %%r2,7" : : : "r0", "r2");
}

__attribute__((noinline, noclone, used)) void prefix_and_gap (void)
{
  __asm__ volatile ("ld.w %%r0,7\n\tld.w %%r1,7\n\tld.w %%r3,7"
                    : : : "r0", "r1", "r3");
}

__attribute__((noinline, noclone, used)) void dense (void)
{
  __asm__ volatile ("ld.w %%r0,7\n\tld.w %%r1,7\n\tld.w %%r2,7\n\tld.w %%r3,7"
                    : : : "r0", "r1", "r2", "r3");
}

/* An independent assembly caller seeds all four preserved registers.
   It does not let the optimizer assume that the tested calls preserve them. */
extern int check_saves (void);
__asm__ (
  ".text\n.balign 2\n.global check_saves\n"
  ".type check_saves,@function\ncheck_saves:\n"
  "pushn %r3\nsub %sp,3\n"
  "ld.w %r0,11\nld.w %r1,12\nld.w %r2,13\nld.w %r3,14\n"
  "xcall singleton\nxcall verify_saves\n"
  "xcall gaps\nxcall verify_saves\n"
  "xcall prefix_and_gap\nxcall verify_saves\n"
  "xcall dense\nxcall verify_saves\n"
  "ld.w %r4,0\nadd %sp,3\npopn %r3\nret\n"
  ".size check_saves,.-check_saves\n"
  ".type verify_saves,@function\nverify_saves:\n"
  "cmp %r0,11\njrne 1f\ncmp %r1,12\njrne 1f\n"
  "cmp %r2,13\njrne 1f\ncmp %r3,14\njrne 1f\nret\n"
  "1: xjp abort\n.size verify_saves,.-verify_saves\n");

int main (void)
{
  return check_saves ();
}
