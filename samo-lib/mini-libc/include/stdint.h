/* C99 fixed-width integer types for the freestanding C33 runtime. */
#ifndef _MINI_LIBC_STDINT_H
#define _MINI_LIBC_STDINT_H

#include <inttypes.h>

#define INT8_MIN   (-127 - 1)
#define INT16_MIN  (-32767 - 1)
#define INT32_MIN  (-2147483647L - 1)
#define INT64_MIN  (-9223372036854775807LL - 1)
#define INT8_MAX   127
#define INT16_MAX  32767
#define INT32_MAX  2147483647L
#define INT64_MAX  9223372036854775807LL
#define UINT8_MAX  255U
#define UINT16_MAX 65535U
#define UINT32_MAX 4294967295UL
#define UINT64_MAX 18446744073709551615ULL

#define INT8_C(v)   v
#define INT16_C(v)  v
#define INT32_C(v)  v##L
#define INT64_C(v)  v##LL
#define UINT8_C(v)  v##U
#define UINT16_C(v) v##U
#define UINT32_C(v) v##UL
#define UINT64_C(v) v##ULL

#define INTPTR_MIN  INT32_MIN
#define INTPTR_MAX  INT32_MAX
#define UINTPTR_MAX UINT32_MAX

#endif
