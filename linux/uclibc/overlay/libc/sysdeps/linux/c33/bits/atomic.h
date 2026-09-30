/* C33's 8/16/32-bit compiler atomics mask interrupts on this single core.
 * The pthread implementation must not recurse into libatomic locks.
 * Licensed under the LGPL v2.1, see COPYING.LIB. */
#ifndef _C33_BITS_ATOMIC_H
#define _C33_BITS_ATOMIC_H
#include <stdint.h>

typedef int8_t atomic8_t;
typedef int16_t atomic16_t;
typedef int32_t atomic32_t;
typedef int64_t atomic64_t;
typedef uint8_t uatomic8_t;
typedef uint16_t uatomic16_t;
typedef uint32_t uatomic32_t;
typedef uint64_t uatomic64_t;
typedef intptr_t atomicptr_t;
typedef uintptr_t uatomicptr_t;
typedef intmax_t atomic_max_t;
typedef uintmax_t uatomic_max_t;

#define __HAVE_64B_ATOMICS 0
#define USE_ATOMIC_COMPILER_BUILTINS 1
#define atomic_compare_and_exchange_val_acq(mem, newval, oldval) \
  ({ __typeof__(*(mem)) __c33_old = (oldval); \
     __atomic_compare_exchange_n((mem), &__c33_old, (newval), 0, \
                                 __ATOMIC_ACQUIRE, __ATOMIC_RELAXED); \
     __c33_old; })
#define atomic_compare_and_exchange_val_rel(mem, newval, oldval) \
  ({ __typeof__(*(mem)) __c33_old = (oldval); \
     __atomic_compare_exchange_n((mem), &__c33_old, (newval), 0, \
                                 __ATOMIC_RELEASE, __ATOMIC_RELAXED); \
     __c33_old; })
#define atomic_exchange_acq(mem, value) \
  __atomic_exchange_n((mem), (value), __ATOMIC_ACQUIRE)
#define atomic_exchange_rel(mem, value) \
  __atomic_exchange_n((mem), (value), __ATOMIC_RELEASE)
#define atomic_exchange_and_add_acq(mem, value) \
  __atomic_fetch_add((mem), (value), __ATOMIC_ACQUIRE)
#define atomic_exchange_and_add_rel(mem, value) \
  __atomic_fetch_add((mem), (value), __ATOMIC_RELEASE)
#define atomic_full_barrier() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define atomic_read_barrier() __atomic_thread_fence(__ATOMIC_ACQUIRE)
#define atomic_write_barrier() __atomic_thread_fence(__ATOMIC_RELEASE)
#endif
