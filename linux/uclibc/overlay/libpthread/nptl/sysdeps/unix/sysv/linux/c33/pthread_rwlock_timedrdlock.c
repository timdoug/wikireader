/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "c33-rwlock.h"

int
pthread_rwlock_timedrdlock (pthread_rwlock_t *rwlock, const struct timespec *abstime)
{
  return __c33_rwlock_lock (rwlock, 0, 0, abstime);
}
