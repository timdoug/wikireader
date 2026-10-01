/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "c33-rwlock.h"

int attribute_protected
__pthread_rwlock_wrlock (pthread_rwlock_t *rwlock)
{
  return __c33_rwlock_lock (rwlock, 1, 0, NULL);
}

weak_alias (__pthread_rwlock_wrlock, pthread_rwlock_wrlock)
strong_alias (__pthread_rwlock_wrlock, __pthread_rwlock_wrlock_internal)
