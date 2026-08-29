/*
 * <setjmp.h> for the C33 DejaGnu board only.  The implementation is in
 * tests/runtime/setjmp.s; the buffer is %r0-%r3, %sp and the return
 * address, which is everything the C33 ABI says survives a call.
 */

#ifndef SETJMP_H
#define SETJMP_H

typedef long jmp_buf[6];

int setjmp (jmp_buf);
void longjmp (jmp_buf, int) __attribute__((noreturn));

/* No signal mask on this target, so these are the same functions.  */
typedef jmp_buf sigjmp_buf;
#define sigsetjmp(env, save) setjmp (env)
#define siglongjmp(env, val) longjmp (env, val)

#endif
