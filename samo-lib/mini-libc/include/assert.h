#ifndef _MINI_LIBC_ASSERT_H
#define _MINI_LIBC_ASSERT_H

#include <stdlib.h>

/* Firmware has no diagnostic stream or process environment. Production
 * builds define NDEBUG; retaining a trap here makes accidental debug builds
 * fail closed instead of continuing after a violated invariant. */
#ifdef NDEBUG
#define assert(expression) ((void)0)
#else
#define assert(expression) ((expression) ? (void)0 : abort())
#endif

#endif
