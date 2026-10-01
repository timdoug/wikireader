/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "io-checks.h"
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <string.h>

static pthread_rwlock_t lock = PTHREAD_RWLOCK_INITIALIZER;
static int order[8], count;
struct worker {
    int id, writer, timed, ready, acquired, result;
    pthread_t thread;
    sem_t release;
};

static void wait_flag(int *flag)
{
    int64_t limit = now_ns() + 3000000000LL;
    while (!__atomic_load_n(flag, __ATOMIC_ACQUIRE) && now_ns() < limit)
        pause_ns(1000000);
    REQUIRE(__atomic_load_n(flag, __ATOMIC_ACQUIRE));
}

static struct timespec deadline(void)
{
    struct timespec ts;
    REQUIRE(clock_gettime(CLOCK_REALTIME, &ts) == 0);
    ts.tv_sec += 3;
    return ts;
}

static void *run(void *arg)
{
    struct worker *w = arg;
    struct timespec ts = deadline();
    __atomic_store_n(&w->ready, 1, __ATOMIC_RELEASE);
    w->result = w->timed ? (w->writer ? pthread_rwlock_timedwrlock(&lock, &ts)
                                        : pthread_rwlock_timedrdlock(&lock, &ts))
                         : (w->writer ? pthread_rwlock_wrlock(&lock)
                                      : pthread_rwlock_rdlock(&lock));
    if (w->result)
        return NULL;
    int index = __atomic_fetch_add(&count, 1, __ATOMIC_SEQ_CST);
    order[index] = w->id;
    __atomic_store_n(&w->acquired, 1, __ATOMIC_RELEASE);
    while (sem_wait(&w->release) != 0)
        REQUIRE(errno == EINTR);
    REQUIRE(pthread_rwlock_unlock(&lock) == 0);
    return NULL;
}

static void start(struct worker *w, int id, int writer, int timed, int policy, int prio)
{
    *w = (struct worker){.id = id, .writer = writer, .timed = timed};
    REQUIRE(sem_init(&w->release, 0, 0) == 0);
    pthread_attr_t attr;
    struct sched_param param = {.sched_priority = prio};
    REQUIRE(pthread_attr_init(&attr) == 0);
    REQUIRE(pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED) == 0);
    REQUIRE(pthread_attr_setschedpolicy(&attr, policy) == 0);
    REQUIRE(pthread_attr_setschedparam(&attr, &param) == 0);
    REQUIRE(pthread_create(&w->thread, &attr, run, w) == 0);
    REQUIRE(pthread_attr_destroy(&attr) == 0);
    wait_flag(&w->ready);
    /* The high-priority main thread sleeps so the worker can enter its
     * blocking lock call. The ready flag alone is not proof of a wait. */
    pause_ns(30000000);
    CHECK(!__atomic_load_n(&w->acquired, __ATOMIC_ACQUIRE));
}

static void finish_worker(struct worker *w)
{
    REQUIRE(sem_post(&w->release) == 0);
    REQUIRE(pthread_join(w->thread, NULL) == 0);
    CHECK(w->result == 0);
    REQUIRE(sem_destroy(&w->release) == 0);
}

static void ordered(int policy, int timed, int change)
{
    struct worker reader, high_writer, low_writer;
    count = 0;
    memset(order, 0, sizeof(order));
    REQUIRE(pthread_rwlock_wrlock(&lock) == 0);
    /* Reader arrives first: equal-priority writer must still win. */
    start(&reader, 2, 0, timed, policy, 3);
    start(&high_writer, 1, 1, timed, policy, change == 1 ? 1 : 3);
    start(&low_writer, 3, 1, timed, policy, 1);
    if (change) {
        struct sched_param p = {.sched_priority = change == 2 ? 1 : 3};
        REQUIRE(pthread_setschedparam(high_writer.thread, policy, &p) == 0);
    }
    REQUIRE(pthread_rwlock_unlock(&lock) == 0);
    struct worker *sequence[] = {change == 2 ? &reader : &high_writer,
                                change == 2 ? &high_writer : &reader, &low_writer};
    for (int i = 0; i < 3; ++i) {
        wait_flag(&sequence[i]->acquired);
        CHECK(order[i] == sequence[i]->id && __atomic_load_n(&count, __ATOMIC_ACQUIRE) == i + 1);
        for (int j = i + 1; j < 3; ++j)
            CHECK(!__atomic_load_n(&sequence[j]->acquired, __ATOMIC_ACQUIRE));
        /* Direct handoff must prevent try-lock callers stealing a writer's grant. */
        if (sequence[i]->writer)
            CHECK(pthread_rwlock_tryrdlock(&lock) == EBUSY);
        CHECK(pthread_rwlock_trywrlock(&lock) == EBUSY);
        finish_worker(sequence[i]);
    }
    printf("policy %d, timed %d, priority change %d: %d %d %d\n",
           policy, timed, change, order[0], order[1], order[2]);
}

static void cohort(int policy, int timed)
{
    struct worker high, middle, writer, low;
    count = 0;
    REQUIRE(pthread_rwlock_wrlock(&lock) == 0);
    start(&low, 4, 0, timed, policy, 1);
    start(&writer, 3, 1, timed, policy, 2);
    start(&middle, 2, 0, timed, policy, 3);
    start(&high, 1, 0, timed, policy, 4);
    REQUIRE(pthread_rwlock_unlock(&lock) == 0);
    wait_flag(&high.acquired);
    wait_flag(&middle.acquired);
    CHECK(!__atomic_load_n(&writer.acquired, __ATOMIC_ACQUIRE));
    CHECK(!__atomic_load_n(&low.acquired, __ATOMIC_ACQUIRE));
    finish_worker(&high);
    CHECK(!__atomic_load_n(&writer.acquired, __ATOMIC_ACQUIRE));
    finish_worker(&middle);
    wait_flag(&writer.acquired);
    CHECK(!__atomic_load_n(&low.acquired, __ATOMIC_ACQUIRE));
    finish_worker(&writer);
    wait_flag(&low.acquired);
    finish_worker(&low);
    CHECK(__atomic_load_n(&count, __ATOMIC_ACQUIRE) == 4);
    printf("policy %d, timed %d: reader cohort above waiting writer\n", policy, timed);
}

static void admission(int policy, int timed)
{
    struct worker writer, peer;
    count = 0;
    struct sched_param p = {.sched_priority = 3};
    REQUIRE(pthread_setschedparam(pthread_self(), policy, &p) == 0);
    REQUIRE(pthread_rwlock_rdlock(&lock) == 0);
    start(&writer, 1, 1, timed, policy, 3);
    /* Existing readers may recurse despite an equal-priority writer. */
    CHECK(pthread_rwlock_rdlock(&lock) == 0);
    CHECK(pthread_rwlock_tryrdlock(&lock) == 0);
    REQUIRE(pthread_rwlock_unlock(&lock) == 0);
    REQUIRE(pthread_rwlock_unlock(&lock) == 0);
    start(&peer, 2, 0, timed, policy, 3);
    REQUIRE(pthread_rwlock_unlock(&lock) == 0);
    wait_flag(&writer.acquired);
    CHECK(!__atomic_load_n(&peer.acquired, __ATOMIC_ACQUIRE));
    finish_worker(&writer);
    wait_flag(&peer.acquired);
    finish_worker(&peer);

    /* A new reader above a waiting writer is permitted to join readers. */
    REQUIRE(pthread_rwlock_rdlock(&lock) == 0);
    start(&writer, 1, 1, timed, policy, 1);
    peer = (struct worker){.id = 2, .timed = timed};
    REQUIRE(sem_init(&peer.release, 0, 0) == 0);
    pthread_attr_t attr;
    p.sched_priority = 4;
    REQUIRE(pthread_attr_init(&attr) == 0);
    REQUIRE(pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED) == 0);
    REQUIRE(pthread_attr_setschedpolicy(&attr, policy) == 0);
    REQUIRE(pthread_attr_setschedparam(&attr, &p) == 0);
    REQUIRE(pthread_create(&peer.thread, &attr, run, &peer) == 0);
    REQUIRE(pthread_attr_destroy(&attr) == 0);
    wait_flag(&peer.acquired);
    CHECK(!__atomic_load_n(&writer.acquired, __ATOMIC_ACQUIRE));
    finish_worker(&peer);
    REQUIRE(pthread_rwlock_unlock(&lock) == 0);
    wait_flag(&writer.acquired);
    finish_worker(&writer);
    count = 0;
    /* Reader preference for ordinary policies must not let an ordinary
     * newcomer barge ahead of a queued realtime writer. */
    p.sched_priority = 0;
    REQUIRE(pthread_setschedparam(pthread_self(), SCHED_OTHER, &p) == 0);
    REQUIRE(pthread_rwlock_rdlock(&lock) == 0);
    start(&writer, 1, 1, timed, policy, 1);
    start(&peer, 2, 0, timed, SCHED_OTHER, 0);
    REQUIRE(pthread_rwlock_unlock(&lock) == 0);
    wait_flag(&writer.acquired);
    CHECK(!__atomic_load_n(&peer.acquired, __ATOMIC_ACQUIRE));
    finish_worker(&writer);
    wait_flag(&peer.acquired);
    finish_worker(&peer);
    p.sched_priority = 10;
    REQUIRE(pthread_setschedparam(pthread_self(), policy, &p) == 0);
    count = 0;
    printf("policy %d, timed %d: recursive and higher-priority admission\n", policy, timed);
}

int main(void)
{
    for (int policy_index = 0; policy_index < 2; ++policy_index) {
        int policy = policy_index ? SCHED_RR : SCHED_FIFO;
        struct sched_param p = {.sched_priority = 10};
        REQUIRE(pthread_setschedparam(pthread_self(), policy, &p) == 0);
        for (int timed = 0; timed < 2; ++timed) {
            ordered(policy, timed, 0);
            ordered(policy, timed, 1);
            ordered(policy, timed, 2);
            cohort(policy, timed);
            admission(policy, timed);
            p.sched_priority = 10;
            REQUIRE(pthread_setschedparam(pthread_self(), policy, &p) == 0);
        }
    }
    CHECK(pthread_rwlock_destroy(&lock) == 0);
    return finish("FIFO/RR rwlock ordering, timed handoff, dynamic priorities and recursive admission");
}
