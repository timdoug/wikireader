/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "c33-rwlock.h"

int
__pthread_rwlock_tryrdlock (pthread_rwlock_t *rwlock)
{
  return __c33_rwlock_lock (rwlock, 0, 1, NULL);
}

strong_alias (__pthread_rwlock_tryrdlock, pthread_rwlock_tryrdlock)
