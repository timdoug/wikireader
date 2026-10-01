/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "io-checks.h"
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <sys/poll.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <unistd.h>

static volatile sig_atomic_t interrupted;
static void handler(int sig)
{
    (void)sig;
    interrupted = 1;
}

int main(void)
{
    int fds[2], high;
    char byte;
    struct pollfd p[3];
    struct rlimit limit, raised;
    struct sigaction action = {0};
    REQUIRE(pipe(fds) == 0);
    p[0] = (struct pollfd){.fd = fds[0], .events = POLLIN};
    CHECK(poll(p, 1, 0) == 0 && p[0].revents == 0);
    CHECK(write(fds[1], "x", 1) == 1);
    CHECK(close(fds[1]) == 0);
    CHECK(poll(p, 1, -2) == 1 && p[0].revents == (POLLIN | POLLHUP));
    CHECK(read(fds[0], &byte, 1) == 1 && byte == 'x');
    CHECK(poll(p, 1, -1) == 1 && p[0].revents == POLLHUP);
    p[0].events = 0; /* Hangup must be reported even if not requested. */
    CHECK(poll(p, 1, 0) == 1 && p[0].revents == POLLHUP);
    p[1] = (struct pollfd){.fd = -1, .events = POLLIN, .revents = -1};
    p[2] = (struct pollfd){.fd = fds[1], .events = 0};
    CHECK(poll(p, 3, 0) == 2 && p[1].revents == 0 && p[2].revents == POLLNVAL);
    CHECK(close(fds[0]) == 0);

    REQUIRE(pipe(fds) == 0);
    CHECK(close(fds[0]) == 0);
    p[0] = (struct pollfd){.fd = fds[1], .events = 0};
    CHECK(poll(p, 1, 0) == 1 && p[0].revents == POLLERR);
    CHECK(close(fds[1]) == 0);
    if (failures)
        return finish("poll hangup and error flags");

    /* Poll has no FD_SETSIZE ceiling; exercise a real high-numbered fd. */
    REQUIRE(getrlimit(RLIMIT_NOFILE, &limit) == 0);
    raised = limit;
    if (raised.rlim_cur <= FD_SETSIZE + 16)
        raised.rlim_cur = FD_SETSIZE + 17;
    REQUIRE(raised.rlim_cur <= raised.rlim_max && setrlimit(RLIMIT_NOFILE, &raised) == 0);
    REQUIRE(pipe(fds) == 0);
    high = fcntl(fds[0], F_DUPFD, FD_SETSIZE + 16);
    REQUIRE(high >= FD_SETSIZE + 16);
    p[0] = (struct pollfd){.fd = high, .events = POLLIN};
    CHECK(write(fds[1], "y", 1) == 1);
    CHECK(poll(p, 1, 0) == 1 && p[0].revents == POLLIN);
    CHECK(read(high, &byte, 1) == 1 && byte == 'y');
    CHECK(close(high) == 0 && close(fds[0]) == 0 && close(fds[1]) == 0);
    CHECK(setrlimit(RLIMIT_NOFILE, &limit) == 0);

    action.sa_handler = handler;
    sigemptyset(&action.sa_mask);
    REQUIRE(sigaction(SIGALRM, &action, NULL) == 0);
    alarm(1);
    errno = 0;
    CHECK(poll(NULL, 0, INT_MAX) == -1 && errno == EINTR && interrupted);
    alarm(0);
    CHECK(poll(NULL, 0, 1) == 0);
    return finish("poll hangup/error flags, negative fds, high fds and interruption");
}
