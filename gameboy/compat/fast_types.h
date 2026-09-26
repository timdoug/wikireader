/* mini-libc's stdint.h stops at the exact-width types; Peanut-GB also
   wants the fast ones.  Anything wider than a byte is a whole register. */
#ifndef WR_GB_FAST_TYPES_H
#define WR_GB_FAST_TYPES_H

#include <stdint.h>

#ifndef INT_FAST16_MAX
typedef int8_t int_fast8_t;
typedef uint8_t uint_fast8_t;
typedef int32_t int_fast16_t;
typedef uint32_t uint_fast16_t;
typedef int32_t int_fast32_t;
typedef uint32_t uint_fast32_t;
#define INT_FAST16_MAX INT32_MAX
#define UINT_FAST16_MAX UINT32_MAX
#define INT_FAST32_MAX INT32_MAX
#define UINT_FAST32_MAX UINT32_MAX
#endif

#endif
