/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "io-checks.h"
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/select.h>
#include <unistd.h>

static volatile sig_atomic_t caught;
static void handler(int signo)
{
    caught = signo;
}

static void readiness(int fd)
{
    struct timespec ts = {2147483999LL, 123456789}, original = ts;
    struct pollfd pfd = {fd, POLLIN, 0};
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    CHECK(pselect(fd + 1, &set, NULL, NULL, &ts, NULL) == 1 && FD_ISSET(fd, &set));
    CHECK(memcmp(&ts, &original, sizeof(ts)) == 0);
    CHECK(ppoll(&pfd, 1, &ts, NULL) == 1 && pfd.revents == POLLIN);
    CHECK(memcmp(&ts, &original, sizeof(ts)) == 0);
    CHECK(poll(&pfd, 1, -1) == 1 && pfd.revents == POLLIN);

    /* Reject invalid input even when a descriptor is already ready. */
    for (int n = 0; n < 2; ++n) {
        ts.tv_nsec = n ? 1000000000 : -1;
        original = ts;
        FD_ZERO(&set);
        FD_SET(fd, &set);
        errno = 0;
        CHECK(pselect(fd + 1, &set, NULL, NULL, &ts, NULL) == -1 && errno == EINVAL);
        CHECK(memcmp(&ts, &original, sizeof(ts)) == 0);
        errno = 0;
        CHECK(ppoll(&pfd, 1, &ts, NULL) == -1 && errno == EINVAL);
        CHECK(memcmp(&ts, &original, sizeof(ts)) == 0);
    }
    const struct timeval invalid[] = {{0, -1}, {-1, 2000000}};
    for (unsigned n = 0; n < sizeof(invalid) / sizeof(invalid[0]); ++n) {
        struct timeval tv = invalid[n];
        FD_ZERO(&set);
        FD_SET(fd, &set);
        errno = 0;
        CHECK(select(fd + 1, &set, NULL, NULL, &tv) == -1 && errno == EINVAL);
        CHECK(memcmp(&tv, &invalid[n], sizeof(tv)) == 0);
    }
    /* Preserve the existing GNU extension for nonnegative microseconds. */
    struct timeval tv = {0, 2000000};
    FD_ZERO(&set);
    FD_SET(fd, &set);
    CHECK(select(fd + 1, &set, NULL, NULL, &tv) == 1 && FD_ISSET(fd, &set));
    /* Normalization must not overflow time_t before reaching the kernel. */
    struct { unsigned long before; struct timeval value; unsigned long after; } huge =
        {0x12345678, {INT64_MAX, 2000000}, 0x87654321};
    FD_ZERO(&set);
    FD_SET(fd, &set);
    CHECK(select(fd + 1, &set, NULL, NULL, &huge.value) == 1 && FD_ISSET(fd, &set));
    CHECK(huge.before == 0x12345678 && huge.after == 0x87654321);
    CHECK(huge.value.tv_sec >= 0 && huge.value.tv_usec >= 0 &&
          huge.value.tv_usec < 1000000);
}

static void expiry(void)
{
    for (int mode = 0; mode < 3; ++mode) {
        struct timespec ts = {0, 30000000}, original = ts;
        struct timeval tv = {0, 30000};
        int64_t before = now_ns();
        int rc = mode == 0 ? pselect(0, NULL, NULL, NULL, &ts, NULL) :
                 mode == 1 ? ppoll(NULL, 0, &ts, NULL) :
                             select(0, NULL, NULL, NULL, &tv);
        int64_t elapsed = now_ns() - before;
        CHECK(rc == 0 && elapsed >= 25000000 && elapsed < 2000000000);
        if (mode < 2)
            CHECK(memcmp(&ts, &original, sizeof(ts)) == 0);
        else
            /* This checks Linux's timeout writeback, not a POSIX mandate. */
            CHECK(tv.tv_sec == 0 && tv.tv_usec == 0);
    }
}

static void interrupted_select(void)
{
    for (int restart = 0; restart < 2; ++restart) {
        struct sigaction action = {.sa_handler = handler,
                                   .sa_flags = restart ? SA_RESTART : 0};
        struct sigevent event = {.sigev_notify = SIGEV_SIGNAL, .sigev_signo = SIGUSR1};
        struct itimerspec setting = {{0, 0}, {0, 30000000}};
        struct { unsigned long before; struct timeval value; unsigned long after; } timeout =
            {0x12345678, {restart ? 0 : 2, restart ? 2000000 : 250000}, 0x87654321};
        int64_t original = timeout.value.tv_sec * 1000000 + timeout.value.tv_usec;
        timer_t timer;
        sigemptyset(&action.sa_mask);
        REQUIRE(sigaction(SIGUSR1, &action, NULL) == 0);
        REQUIRE(timer_create(CLOCK_MONOTONIC, &event, &timer) == 0);
        caught = 0;
        REQUIRE(timer_settime(timer, 0, &setting, NULL) == 0);
        int64_t before = now_ns();
        errno = 0;
        int rc = select(0, NULL, NULL, NULL, &timeout.value);
        int error = errno;
        int64_t elapsed = now_ns() - before;
        REQUIRE(timer_delete(timer) == 0);
        CHECK(rc == -1 && error == EINTR && caught == SIGUSR1);
        CHECK(elapsed >= 25000000 && elapsed < 2000000000);
        CHECK(timeout.before == 0x12345678 && timeout.after == 0x87654321);
        int64_t remaining = timeout.value.tv_sec * 1000000 + timeout.value.tv_usec;
        CHECK(timeout.value.tv_sec >= 0 && timeout.value.tv_usec >= 0 &&
              timeout.value.tv_usec < 1000000 &&
              remaining < original - 20000 && remaining > original - 1000000);
    }
}

static void pending_signal(int signo, int mode)
{
    struct sigaction action = {.sa_handler = handler};
    sigset_t block, saved, during, after;
    struct timespec ts = {2, 123456789}, original = ts;
    sigemptyset(&action.sa_mask);
    REQUIRE(sigaction(signo, &action, NULL) == 0);
    sigemptyset(&block);
    sigaddset(&block, signo);
    REQUIRE(pthread_sigmask(SIG_BLOCK, &block, &saved) == 0);
    REQUIRE(pthread_sigmask(SIG_SETMASK, NULL, &during) == 0);
    sigdelset(&during, signo);
    caught = 0;
    REQUIRE(raise(signo) == 0);
    CHECK(caught == 0);
    errno = 0;
    int ep = -1;
    struct epoll_event event;
    if (mode == 2) {
        ep = epoll_create1(EPOLL_CLOEXEC);
        REQUIRE(ep >= 0);
    }
    int rc = mode == 0 ? pselect(0, NULL, NULL, NULL, &ts, &during) :
             mode == 1 ? ppoll(NULL, 0, &ts, &during) :
                         epoll_pwait(ep, &event, 1, 2000, &during);
    CHECK(rc == -1 && errno == EINTR && caught == signo);
    CHECK(memcmp(&ts, &original, sizeof(ts)) == 0);
    CHECK(pthread_sigmask(SIG_SETMASK, NULL, &after) == 0 &&
          sigismember(&after, signo) == 1);
    REQUIRE(pthread_sigmask(SIG_SETMASK, &saved, NULL) == 0);
    if (ep >= 0)
        CHECK(close(ep) == 0);
}

int main(void)
{
    int fd[2];
    char byte;
    REQUIRE(pipe(fd) == 0);
    REQUIRE(write(fd[1], "x", 1) == 1);
    readiness(fd[0]);
    CHECK(read(fd[0], &byte, 1) == 1 && byte == 'x');
    expiry();
    interrupted_select();
    for (int mode = 0; mode < 3; ++mode) {
        pending_signal(SIGUSR1, mode);
        pending_signal(SIGRTMIN + 5, mode);
    }
    CHECK(close(fd[0]) == 0 && close(fd[1]) == 0);
    return finish("wait readiness, wide/invalid timeouts, expiry and atomic signal masks");
}
