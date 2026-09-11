/* Private compiler error recovery; GCC's builtins save a 5-word context.
 * This is not installed as the target C library's public setjmp.h.
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
#ifndef TCC_NUTTX_SETJMP_H
#define TCC_NUTTX_SETJMP_H
#include <stdint.h>
typedef intptr_t jmp_buf[5];
#define setjmp(buf) __builtin_setjmp(buf)
#define longjmp(buf, val) __builtin_longjmp(buf, 1)
#endif
