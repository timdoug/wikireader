/* { dg-do compile } */
/* { dg-options "-O2 -mc33pe -std=gnu17 -fno-optimize-sibling-calls" } */

extern int typed (int, int);
extern int vari (int, ...);
extern int unproto ();

int typed_call (int a) { return typed (a, 2) + 1; }
int vari_call (int a) { return vari (a, 3) + 1; }
int unproto_call (int a) { return unproto (a) + 1; }

/* The __builtin_apply forwarding descriptor (0xc3000000 and up, printed
   as -10...) goes only to variadic and unprototyped callees.  */
/* { dg-final { scan-assembler-times "xld.w\t%r5,-10" 2 } } */
