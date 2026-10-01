/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "io-checks.h"
#include <linux/futex.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>

/* Both 32-bit time64 and native 64-bit kernels use these two 64-bit words. */
struct kernel_timeout { int64_t seconds, nanoseconds; };
#ifdef SYS_futex_time64
#define FUTEX_SYSCALL SYS_futex_time64
#else
#define FUTEX_SYSCALL SYS_futex
#endif

static unsigned word;
static long futex(int op, unsigned value, struct kernel_timeout *timeout, unsigned mask)
{
    return syscall(FUTEX_SYSCALL, &word, op | FUTEX_PRIVATE_FLAG,
                   value, timeout, NULL, mask);
}

static void deadlines(void)
{
    struct kernel_timeout timeout = {2147483999LL, 123456789};
    errno = 0;
    CHECK(futex(FUTEX_WAIT, 1, &timeout, 0) == -1 && errno == EAGAIN);
    for (int invalid = 0; invalid < 2; ++invalid) {
        timeout.seconds = 0;
        timeout.nanoseconds = invalid ? 1000000000 : -1;
        errno = 0;
        CHECK(futex(FUTEX_WAIT, 0, &timeout, 0) == -1 && errno == EINVAL);
    }
    for (int mode = 0; mode < 3; ++mode) {
        int op = mode ? FUTEX_WAIT_BITSET : FUTEX_WAIT;
        clockid_t clock = mode == 2 ? CLOCK_REALTIME : CLOCK_MONOTONIC;
        timeout.seconds = 0;
        timeout.nanoseconds = 30000000;
        if (mode) {
            struct timespec now;
            REQUIRE(clock_gettime(clock, &now) == 0);
            timeout.seconds += now.tv_sec;
            timeout.nanoseconds += now.tv_nsec;
            if (timeout.nanoseconds >= 1000000000) {
                ++timeout.seconds;
                timeout.nanoseconds -= 1000000000;
            }
        }
        if (mode == 2)
            op |= FUTEX_CLOCK_REALTIME;
        int64_t before = now_ns();
        errno = 0;
        CHECK(futex(op, 0, &timeout, FUTEX_BITSET_MATCH_ANY) == -1 && errno == ETIMEDOUT);
        int64_t elapsed = now_ns() - before;
        CHECK(elapsed >= 25000000 && elapsed < 2000000000);
    }
    timeout.seconds = timeout.nanoseconds = 0;
    errno = 0;
    CHECK(futex(FUTEX_WAIT_BITSET, 0, &timeout, FUTEX_BITSET_MATCH_ANY) == -1 &&
          errno == ETIMEDOUT);
    errno = 0;
    CHECK(futex(FUTEX_WAIT_BITSET, 0, &timeout, 0) == -1 && errno == EINVAL);
}

struct waiter { int ready, result, error; unsigned mask; };
static void *wait_for_bit(void *arg)
{
    struct waiter *waiter = arg;
    struct timespec now;
    REQUIRE(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    struct kernel_timeout timeout = {now.tv_sec + 2, now.tv_nsec};
    REQUIRE(write(waiter->ready, "r", 1) == 1);
    waiter->result = futex(FUTEX_WAIT_BITSET, 0, &timeout, waiter->mask);
    waiter->error = errno;
    return NULL;
}

int main(void)
{
    deadlines();
    int ready[2];
    char bytes[2];
    pthread_t threads[2];
    REQUIRE(pipe(ready) == 0);
    struct waiter waiters[] = {{ready[1], -1, 0, 2}, {ready[1], -1, 0, 4}};
    for (int n = 0; n < 2; ++n)
        REQUIRE(pthread_create(&threads[n], NULL, wait_for_bit, &waiters[n]) == 0);
    for (int n = 0; n < 2; ++n)
        REQUIRE(read(ready[0], &bytes[n], 1) == 1 && bytes[n] == 'r');
    pause_ns(30000000);
    CHECK(futex(FUTEX_WAKE_BITSET, 1, NULL, 2) == 1);
    REQUIRE(pthread_join(threads[0], NULL) == 0);
    CHECK(waiters[0].result == 0);
    CHECK(futex(FUTEX_WAKE_BITSET, 1, NULL, 4) == 1);
    REQUIRE(pthread_join(threads[1], NULL) == 0);
    CHECK(waiters[1].result == 0);
    CHECK(futex(FUTEX_WAKE, 1, NULL, 0) == 0);
    CHECK(close(ready[0]) == 0 && close(ready[1]) == 0);
    return finish("raw time64 futex validation, relative/absolute deadlines and selective wakeups");
}
