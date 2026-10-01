/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "c33-rwlock.h"
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <lowlevellock.h>

/* C33 Linux has one physical address space. Queue nodes on a waiter's
   stack and owner records in its TLS/heap are therefore also accessible
   to another process using a process-shared lock. This implementation
   must not be used on an MMU target.

   Keep the 32-byte public object and its zero/static initializers:
   __readers_wakeup now holds the wait-list head; __writer_wakeup holds
   the reader-owner list. All list access, grants and reference counts
   are protected by __lock. No allocation is needed for waiting writers.
   Four simultaneous distinct read locks per thread use embedded TLS;
   further locks allocate owner records, with EAGAIN on exhaustion. */
struct reader_owner
{
  struct reader_owner *next;
  int tid;
  unsigned int count;
  int allocated;
};

struct waiter
{
  struct waiter *next;
  struct reader_owner *owner;
  int tid;
  int writer;
  int granted;
  int priority;
};

static __thread struct reader_owner local_owners[4];

static struct waiter *
waiters (pthread_rwlock_t *rwlock)
{
  return (struct waiter *) (uintptr_t) rwlock->__data.__readers_wakeup;
}

static void
set_waiters (pthread_rwlock_t *rwlock, struct waiter *head)
{
  rwlock->__data.__readers_wakeup = (uintptr_t) head;
}

static struct reader_owner *
owners (pthread_rwlock_t *rwlock)
{
  return (struct reader_owner *) (uintptr_t) rwlock->__data.__writer_wakeup;
}

static void
set_owners (pthread_rwlock_t *rwlock, struct reader_owner *head)
{
  rwlock->__data.__writer_wakeup = (uintptr_t) head;
}

/* sched_getparam reports the current assigned RT priority (zero for
   ordinary Linux policies). Query at admission/handoff, not enqueue,
   so changing the priority of a blocked thread is taken into account. */
static int
priority (int tid)
{
  struct sched_param param;
  INTERNAL_SYSCALL_DECL (err);
  int ret = INTERNAL_SYSCALL (sched_getparam, err, 2, tid, &param);
  return INTERNAL_SYSCALL_ERROR_P (ret, err) ? 0 : param.sched_priority;
}

static struct reader_owner *
find_owner (pthread_rwlock_t *rwlock, int tid)
{
  struct reader_owner *owner;
  for (owner = owners (rwlock); owner; owner = owner->next)
    if (owner->tid == tid)
      return owner;
  return NULL;
}

static struct reader_owner *
new_owner (int tid)
{
  struct reader_owner *owner = NULL;
  unsigned int i;
  for (i = 0; i < sizeof (local_owners) / sizeof (local_owners[0]); ++i)
    if (local_owners[i].count == 0)
      {
        owner = &local_owners[i];
        break;
      }
  if (owner == NULL)
    {
      owner = malloc (sizeof (*owner));
      if (owner == NULL)
        return NULL;
      owner->allocated = 1;
    }
  owner->tid = tid;
  owner->count = 1;
  return owner;
}

static void
release_owner (struct reader_owner *owner)
{
  if (owner->allocated)
    free (owner);
  else
    owner->count = 0;
}

static void
add_owner (pthread_rwlock_t *rwlock, struct reader_owner *owner)
{
  owner->next = owners (rwlock);
  set_owners (rwlock, owner);
  ++rwlock->__data.__nr_readers;
}

static int
writer_priority (pthread_rwlock_t *rwlock)
{
  struct waiter *node;
  int highest = -1;
  for (node = waiters (rwlock); node; node = node->next)
    if (node->writer)
      {
        int p = priority (node->tid);
        if (p > highest)
          highest = p;
      }
  return highest;
}

static int
reader_allowed (pthread_rwlock_t *rwlock, int tid)
{
  int p, highest;
  if (rwlock->__data.__writer)
    return 0;
  if (!rwlock->__data.__nr_writers_queued || find_owner (rwlock, tid))
    return 1;
  p = priority (tid);
  highest = writer_priority (rwlock);
  if (p > 0 || highest > 0)
    return p > highest;
  return PTHREAD_RWLOCK_PREFER_READER_P (rwlock);
}

static void
remove_waiter (pthread_rwlock_t *rwlock, struct waiter *node,
               struct waiter *previous)
{
  if (previous)
    previous->next = node->next;
  else
    set_waiters (rwlock, node->next);
  if (node->writer)
    --rwlock->__data.__nr_writers_queued;
  else
    --rwlock->__data.__nr_readers_queued;
}

static void
grant (pthread_rwlock_t *rwlock, struct waiter *node, struct waiter *previous)
{
  remove_waiter (rwlock, node, previous);
  if (node->writer)
    rwlock->__data.__writer = node->tid;
  else
    add_owner (rwlock, node->owner);
  node->granted = 1;
  /* Wake before releasing __lock: the waiter must reacquire it before
     returning, so its stack cannot disappear under this futex wake. */
  lll_futex_wake (&node->granted, 1, rwlock->__data.__shared);
}

static void
handoff (pthread_rwlock_t *rwlock)
{
  struct waiter *node, *previous, *best = NULL, *best_previous = NULL;
  int best_priority = -1;
  int max_writer = -1;
  if (rwlock->__data.__writer)
    return;
  previous = NULL;
  for (node = waiters (rwlock); node; previous = node, node = node->next)
    {
      int p = node->priority = priority (node->tid);
      if (node->writer && p > max_writer)
        max_writer = p;
      if (best == NULL || p > best_priority
          || (p == best_priority && node->writer && !best->writer))
        {
          best = node;
          best_previous = previous;
          best_priority = p;
        }
    }
  if (rwlock->__data.__nr_readers == 0 && best && best->writer)
    {
      grant (rwlock, best, best_previous);
      return;
    }
  /* Readers can share the lock only while no waiting writer has equal
     or higher RT priority. With no writers, release every reader. */
  previous = NULL;
  node = waiters (rwlock);
  while (node)
    {
      struct waiter *next = node->next;
      if (!node->writer && node->priority > max_writer
          && rwlock->__data.__nr_readers != UINT_MAX)
        grant (rwlock, node, previous);
      else
        previous = node;
      node = next;
    }
}

int
__c33_rwlock_lock (pthread_rwlock_t *rwlock, int writer, int attempt,
                   const struct timespec *abstime)
{
  int tid = THREAD_GETMEM (THREAD_SELF, tid);
  int saved_errno = errno;
  int result = 0;
  struct reader_owner *owner = NULL;
  struct waiter node = { NULL, NULL, tid, writer, 0, 0 };
  struct waiter *tail;
  unsigned int *queued = writer ? &rwlock->__data.__nr_writers_queued
                               : &rwlock->__data.__nr_readers_queued;
  lll_lock (rwlock->__data.__lock, rwlock->__data.__shared);
  if (!writer && reader_allowed (rwlock, tid))
    {
      owner = find_owner (rwlock, tid);
      if (rwlock->__data.__nr_readers == UINT_MAX)
        result = EAGAIN;
      else if (owner)
        {
          ++owner->count;
          ++rwlock->__data.__nr_readers;
        }
      else if ((owner = new_owner (tid)) == NULL)
        result = EAGAIN;
      else
        add_owner (rwlock, owner);
      goto done;
    }
  if (writer && rwlock->__data.__writer == 0
      && rwlock->__data.__nr_readers == 0)
    {
      rwlock->__data.__writer = tid;
      goto done;
    }
  if (attempt)
    {
      result = EBUSY;
      goto done;
    }
  if (rwlock->__data.__writer == tid)
    {
      result = EDEADLK;
      goto done;
    }
  if (abstime && (abstime->tv_nsec < 0 || abstime->tv_nsec >= 1000000000))
    {
      result = EINVAL;
      goto done;
    }
  if (abstime && abstime->tv_sec < 0)
    {
      result = ETIMEDOUT;
      goto done;
    }
  if (*queued == UINT_MAX || (!writer && rwlock->__data.__nr_readers == UINT_MAX))
    {
      result = EAGAIN;
      goto done;
    }
  if (!writer)
    {
      node.owner = new_owner (tid);
      if (node.owner == NULL)
        {
          result = EAGAIN;
          goto done;
        }
    }
  ++*queued;
  tail = waiters (rwlock);
  if (!tail)
    set_waiters (rwlock, &node);
  else
    {
      while (tail->next)
        tail = tail->next;
      tail->next = &node;
    }
  while (!node.granted)
    {
      int ret;
      lll_unlock (rwlock->__data.__lock, rwlock->__data.__shared);
      if (abstime)
        {
          /* Absolute realtime time64: no subtraction overflow, no
             truncation after 2038, and clock changes remain visible. */
          struct __ts64_struct deadline = { abstime->tv_sec, abstime->tv_nsec };
          INTERNAL_SYSCALL_DECL (err);
          ret = INTERNAL_SYSCALL (futex_time64, err, 6, &node.granted,
                  __lll_private_flag (FUTEX_WAIT_BITSET | FUTEX_CLOCK_REALTIME,
                                      rwlock->__data.__shared),
                  0, &deadline, NULL, FUTEX_BITSET_MATCH_ANY);
        }
      else
        ret = lll_futex_wait (&node.granted, 0, rwlock->__data.__shared);
      lll_lock (rwlock->__data.__lock, rwlock->__data.__shared);
      /* A committed grant wins a simultaneous timeout. Other futex
         returns (including EINTR and spurious wakes) resume waiting. */
      if (!node.granted && (ret == -ETIMEDOUT || ret == -EINVAL))
        {
          struct waiter *previous = NULL;
          struct waiter *cursor;
          for (cursor = waiters (rwlock); cursor != &node; cursor = cursor->next)
            previous = cursor;
          remove_waiter (rwlock, &node, previous);
          if (node.owner)
            release_owner (node.owner);
          handoff (rwlock);
          result = -ret;
          break;
        }
    }
done:
  lll_unlock (rwlock->__data.__lock, rwlock->__data.__shared);
  errno = saved_errno;
  return result;
}

int attribute_protected
__pthread_rwlock_unlock (pthread_rwlock_t *rwlock)
{
  int saved_errno = errno;
  lll_lock (rwlock->__data.__lock, rwlock->__data.__shared);
  if (rwlock->__data.__writer)
    rwlock->__data.__writer = 0;
  else
    {
      struct reader_owner *owner = owners (rwlock), *previous = NULL;
      int tid = THREAD_GETMEM (THREAD_SELF, tid);
      while (owner->tid != tid)
        {
          previous = owner;
          owner = owner->next;
        }
      --rwlock->__data.__nr_readers;
      if (--owner->count == 0)
        {
          if (previous)
            previous->next = owner->next;
          else
            set_owners (rwlock, owner->next);
          release_owner (owner);
        }
    }
  if (waiters (rwlock))
    handoff (rwlock);
  lll_unlock (rwlock->__data.__lock, rwlock->__data.__shared);
  errno = saved_errno;
  return 0;
}
weak_alias (__pthread_rwlock_unlock, pthread_rwlock_unlock)
strong_alias (__pthread_rwlock_unlock, __pthread_rwlock_unlock_internal)
