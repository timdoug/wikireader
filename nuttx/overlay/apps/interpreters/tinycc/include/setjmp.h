/* SPDX-License-Identifier: LGPL-2.0-or-later */
#ifndef _TCC_SETJMP_H
#define _TCC_SETJMP_H
typedef unsigned int jmp_buf[6];
int tcc_c33_setjmp(jmp_buf) __attribute__((returns_twice));
void tcc_c33_longjmp(jmp_buf, int) __attribute__((noreturn));
#define setjmp(buf) tcc_c33_setjmp(buf)
#define longjmp(buf, value) tcc_c33_longjmp(buf, value)
#endif
