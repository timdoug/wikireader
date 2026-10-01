/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "c33-rwlock.h"

int attribute_protected
__pthread_rwlock_rdlock (pthread_rwlock_t *rwlock)
{
  return __c33_rwlock_lock (rwlock, 0, 0, NULL);
}

weak_alias (__pthread_rwlock_rdlock, pthread_rwlock_rdlock)
strong_alias (__pthread_rwlock_rdlock, __pthread_rwlock_rdlock_internal)
