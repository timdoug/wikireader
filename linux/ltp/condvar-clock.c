/* Distinguish clock selection from successful attribute round-trips. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>

static int failures;
#define CHECK(condition) do { \
    if (!(condition)) { \
        printf("FAIL line %d: %s (errno %d)\n", __LINE__, #condition, errno); \
        ++failures; \
    } \
} while (0)

static long long elapsed_ns(const struct timespec *start,
                            const struct timespec *end)
{
    return (end->tv_sec - start->tv_sec) * 1000000000LL +
           end->tv_nsec - start->tv_nsec;
}

static void test_clock(clockid_t clock)
{
    pthread_condattr_t attr;
    pthread_mutexattr_t mutex_attr;
    pthread_cond_t cond;
    pthread_mutex_t mutex;
    struct timespec deadline, before, after;
    int rc;

    CHECK(pthread_condattr_init(&attr) == 0);
    CHECK(pthread_condattr_setclock(&attr, clock) == 0);
    CHECK(pthread_cond_init(&cond, &attr) == 0);
    CHECK(pthread_mutexattr_init(&mutex_attr) == 0);
    CHECK(pthread_mutexattr_settype(&mutex_attr, PTHREAD_MUTEX_ERRORCHECK) == 0);
    CHECK(pthread_mutex_init(&mutex, &mutex_attr) == 0);
    CHECK(pthread_mutex_lock(&mutex) == 0);
    CHECK(clock_gettime(clock, &deadline) == 0);
    deadline.tv_nsec += 100000000;
    if (deadline.tv_nsec >= 1000000000) {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000000000;
    }
    CHECK(clock_gettime(CLOCK_MONOTONIC, &before) == 0);
    rc = pthread_cond_timedwait(&cond, &mutex, &deadline);
    CHECK(clock_gettime(CLOCK_MONOTONIC, &after) == 0);
    printf("clock %d: result %d, elapsed %lld ns\n", (int)clock, rc,
           elapsed_ns(&before, &after));
    CHECK(rc == ETIMEDOUT);
    CHECK(elapsed_ns(&before, &after) >= 80000000 &&
          elapsed_ns(&before, &after) < 3000000000LL);
    /* ERRORCHECK makes this fail if timedwait did not reacquire the mutex. */
    CHECK(pthread_mutex_unlock(&mutex) == 0);
    CHECK(pthread_mutex_destroy(&mutex) == 0);
    CHECK(pthread_cond_destroy(&cond) == 0);
    CHECK(pthread_mutexattr_destroy(&mutex_attr) == 0);
    CHECK(pthread_condattr_destroy(&attr) == 0);
}

int main(void)
{
    struct timespec realtime, monotonic, shifted, now;
    long long elapsed;
    CHECK(clock_gettime(CLOCK_REALTIME, &realtime) == 0);
    CHECK(clock_gettime(CLOCK_MONOTONIC, &monotonic) == 0);
    /* This local regression runs as root in a disposable emulator boot.
     * Separate the clocks so a realtime fallback cannot appear correct. */
    shifted = realtime;
    shifted.tv_sec += 3600;
    if (clock_settime(CLOCK_REALTIME, &shifted) != 0) {
        perror("clock_settime");
        return 1;
    }
    test_clock(CLOCK_REALTIME);
    test_clock(CLOCK_MONOTONIC);
    CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    elapsed = elapsed_ns(&monotonic, &now);
    realtime.tv_sec += elapsed / 1000000000;
    realtime.tv_nsec += elapsed % 1000000000;
    if (realtime.tv_nsec >= 1000000000) {
        ++realtime.tv_sec;
        realtime.tv_nsec -= 1000000000;
    }
    CHECK(clock_settime(CLOCK_REALTIME, &realtime) == 0);
    if (!failures)
        puts("PASS: realtime and monotonic condition deadlines, mutex reacquisition");
    return failures ? 1 : 0;
}
