/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "io-checks.h"
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

static void event_counter(int ep)
{
    int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    uint64_t value = 12, readback = 0;
    unsigned short short_buffer = 0;
    struct epoll_event event = {.events = EPOLLIN | EPOLLONESHOT,
                               .data.u64 = UINT64_C(0x1234567887654321)};
    struct { unsigned long before; struct epoll_event event; unsigned long after; } output =
        {0xabcdef01, {0}, 0x10fedcba};
    REQUIRE(fd >= 0);
    CHECK(fcntl(fd, F_GETFD) == FD_CLOEXEC);
    CHECK(fcntl(fd, F_GETFL) & O_NONBLOCK);
    errno = 0;
    CHECK(read(fd, &readback, sizeof(readback)) == -1 && errno == EAGAIN);
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, fd, &event) == 0);
    CHECK(write(fd, &value, sizeof(value)) == sizeof(value));
    CHECK(epoll_wait(ep, &output.event, 1, 1000) == 1);
    CHECK(output.before == 0xabcdef01 && output.after == 0x10fedcba &&
          output.event.events == EPOLLIN && output.event.data.u64 == event.data.u64);
    CHECK(epoll_wait(ep, &output.event, 1, 0) == 0);
    CHECK(epoll_ctl(ep, EPOLL_CTL_MOD, fd, &event) == 0);
    CHECK(epoll_wait(ep, &output.event, 1, 0) == 1);
    CHECK(read(fd, &readback, sizeof(readback)) == sizeof(readback) && readback == 12);
    errno = 0;
    CHECK(write(fd, &short_buffer, sizeof(short_buffer)) == -1 && errno == EINVAL);
    value = UINT64_MAX;
    errno = 0;
    CHECK(write(fd, &value, sizeof(value)) == -1 && errno == EINVAL);
    value = UINT64_MAX - 1;
    CHECK(write(fd, &value, sizeof(value)) == sizeof(value));
    value = 1;
    errno = 0;
    CHECK(write(fd, &value, sizeof(value)) == -1 && errno == EAGAIN);
    CHECK(read(fd, &readback, sizeof(readback)) == sizeof(readback) &&
          readback == UINT64_MAX - 1);
    CHECK(close(fd) == 0);
    CHECK(epoll_wait(ep, &output.event, 1, 0) == 0);

    fd = eventfd(2, EFD_NONBLOCK | EFD_SEMAPHORE);
    REQUIRE(fd >= 0);
    for (int n = 0; n < 2; ++n)
        CHECK(read(fd, &readback, sizeof(readback)) == sizeof(readback) && readback == 1);
    errno = 0;
    CHECK(read(fd, &readback, sizeof(readback)) == -1 && errno == EAGAIN);
    CHECK(close(fd) == 0);
}

static void timer_events(int ep, clockid_t clock)
{
    int fd = timerfd_create(clock, TFD_NONBLOCK | TFD_CLOEXEC);
    struct itimerspec setting = {{0, 10000000}, {0, 10000000}};
    struct epoll_event event = {.events = EPOLLIN, .data.u64 = 0xabcdef01};
    struct pollfd pfd = {fd, POLLIN, 0};
    uint64_t count = 0;
    char short_buffer;
    REQUIRE(fd >= 0);
    CHECK(fcntl(fd, F_GETFD) == FD_CLOEXEC);
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, fd, &event) == 0);
    CHECK(timerfd_settime(fd, 0, &setting, NULL) == 0);
    pause_ns(60000000);
    CHECK(epoll_wait(ep, &event, 1, 1000) == 1 && event.events == EPOLLIN &&
          event.data.u64 == 0xabcdef01);
    CHECK(poll(&pfd, 1, 0) == 1 && pfd.revents == POLLIN);
    errno = 0;
    CHECK(read(fd, &short_buffer, 1) == -1 && errno == EINVAL);
    CHECK(read(fd, &count, sizeof(count)) == sizeof(count) && count >= 3);
    /* Disarm before testing EAGAIN, avoiding a race with another tick. */
    memset(&setting, 0, sizeof(setting));
    CHECK(timerfd_settime(fd, 0, &setting, NULL) == 0);
    errno = 0;
    CHECK(read(fd, &count, sizeof(count)) == -1 && errno == EAGAIN);
    CHECK(poll(&pfd, 1, 0) == 0);

    CHECK(clock_gettime(clock, &setting.it_value) == 0);
    setting.it_value.tv_nsec += 30000000;
    if (setting.it_value.tv_nsec >= 1000000000) {
        ++setting.it_value.tv_sec;
        setting.it_value.tv_nsec -= 1000000000;
    }
    CHECK(timerfd_settime(fd, TFD_TIMER_ABSTIME, &setting, NULL) == 0);
    CHECK(epoll_wait(ep, &event, 1, 1000) == 1 && event.events == EPOLLIN);
    CHECK(read(fd, &count, sizeof(count)) == sizeof(count) && count == 1);
    CHECK(close(fd) == 0);
}

int main(void)
{
    for (int size = 0; size >= -1; --size) {
        errno = 0;
        int invalid = epoll_create(size);
        CHECK(invalid == -1 && errno == EINVAL);
        if (invalid >= 0)
            CHECK(close(invalid) == 0);
    }
    int legacy = epoll_create(1);
    REQUIRE(legacy >= 0);
    CHECK(fcntl(legacy, F_GETFD) == 0);
    CHECK(close(legacy) == 0);
    int ep = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event event;
    REQUIRE(ep >= 0);
    CHECK(fcntl(ep, F_GETFD) == FD_CLOEXEC);
    errno = 0;
    CHECK(epoll_wait(ep, &event, 0, 0) == -1 && errno == EINVAL);
    event_counter(ep);
    timer_events(ep, CLOCK_MONOTONIC);
    timer_events(ep, CLOCK_REALTIME);
    CHECK(close(ep) == 0);
    return finish("eventfd counters/overflow, epoll one-shot/rearm and timerfd polling");
}
