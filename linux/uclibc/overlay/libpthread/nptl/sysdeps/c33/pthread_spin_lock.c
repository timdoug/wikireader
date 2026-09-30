/* pthread_spin_lock -- lock a spin lock.  Generic version.
   Copyright (C) 2012-2016 Free Software Foundation, Inc.
   This file is part of the GNU C Library.

   The GNU C Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   The GNU C Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with the GNU C Library; if not, see
   <http://www.gnu.org/licenses/>.  */

#include <atomic.h>
#include <sched.h>
#include "pthreadP.h"

int
pthread_spin_lock (pthread_spinlock_t *lock)
{
  /* atomic_exchange usually takes less instructions than
     atomic_compare_and_exchange.  On the other hand,
     atomic_compare_and_exchange potentially generates less bus traffic
     when the lock is locked.
     We assume that the first try mostly will be successful, and we use
     atomic_exchange.  For the subsequent tries we use
     atomic_compare_and_exchange.  */
  if (atomic_exchange_acq (lock, 1) == 0)
    return 0;

  /* On this single core a contended owner cannot run until we yield or
     are preempted. Keep the uncontended path entirely in userspace. */
  do
    sched_yield();
  while (atomic_compare_and_exchange_val_acq (lock, 1, 0) != 0);

  return 0;
}
