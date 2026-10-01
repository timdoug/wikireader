/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef C33_RWLOCK_H
#define C33_RWLOCK_H
#include <pthreadP.h>
#include <stddef.h>
extern int __c33_rwlock_lock (pthread_rwlock_t *, int, int,
                              const struct timespec *) attribute_hidden;
#endif
