#ifndef _BITS_SETJMP_H
#define _BITS_SETJMP_H 1

#if !defined _SETJMP_H && !defined _PTHREAD_H
# error "Never include <bits/setjmp.h> directly; use <setjmp.h> instead."
#endif

/* r0-r3, the caller's stack pointer, and the return address. */
typedef int __jmp_buf[6];

#endif
