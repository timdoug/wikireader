/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "io-checks.h"
#include <poll.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

enum operation { READ, POLL, PPOLL, SELECT, PSELECT, RECV, RECVMMSG, EPOLL, EPOLL_PWAIT, OPERATIONS };
static const char *const names[] = {
    "read", "poll", "ppoll", "select", "pselect", "recv", "recvmmsg", "epoll_wait", "epoll_pwait"
};
static __thread int tls_value;
struct waiter {
    enum operation operation;
    int fd, ready, ep, pending, cleaned, tls_ok;
};

static void cleanup(void *arg)
{
    struct waiter *waiter = arg;
    ++waiter->cleaned;
    waiter->tls_ok = tls_value == 123;
}

static void *wait_for_io(void *arg)
{
    struct waiter *waiter = arg;
    struct pollfd pfd = {waiter->fd, POLLIN, 0};
    struct epoll_event event;
    char byte;
    struct iovec vec = {&byte, 1};
    struct mmsghdr message = {.msg_hdr = {.msg_iov = &vec, .msg_iovlen = 1}};
    fd_set set;
    FD_ZERO(&set);
    FD_SET(waiter->fd, &set);
    tls_value = 123;
    pthread_cleanup_push(cleanup, waiter);
    if (waiter->pending)
        REQUIRE(pthread_cancel(pthread_self()) == 0);
    else
        REQUIRE(write(waiter->ready, "r", 1) == 1);
    switch (waiter->operation) {
    case READ: read(waiter->fd, &byte, 1); break;
    case POLL: poll(&pfd, 1, -1); break;
    case PPOLL: ppoll(&pfd, 1, NULL, NULL); break;
    case SELECT: select(waiter->fd + 1, &set, NULL, NULL, NULL); break;
    case PSELECT: pselect(waiter->fd + 1, &set, NULL, NULL, NULL, NULL); break;
    case RECV: recv(waiter->fd, &byte, 1, 0); break;
    case RECVMMSG: recvmmsg(waiter->fd, &message, 1, 0, NULL); break;
    case EPOLL: epoll_wait(waiter->ep, &event, 1, -1); break;
    case EPOLL_PWAIT: epoll_pwait(waiter->ep, &event, 1, -1, NULL); break;
    default: break;
    }
    pthread_cleanup_pop(0);
    return (void *)1;
}

int main(void)
{
    for (int operation = 0; operation < OPERATIONS; ++operation) {
        for (int pending = 0; pending < 2; ++pending) {
            int fd[2], ready[2];
            pthread_t thread;
            void *result = NULL;
            REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0);
            REQUIRE(pipe(ready) == 0);
            struct waiter waiter = {operation, fd[0], ready[1], -1, pending, 0, 0};
            if (operation == EPOLL || operation == EPOLL_PWAIT) {
                waiter.ep = epoll_create1(EPOLL_CLOEXEC);
                struct epoll_event event = {.events = EPOLLIN, .data.fd = fd[0]};
                REQUIRE(waiter.ep >= 0);
                REQUIRE(epoll_ctl(waiter.ep, EPOLL_CTL_ADD, fd[0], &event) == 0);
            }
            REQUIRE(pthread_create(&thread, NULL, wait_for_io, &waiter) == 0);
            if (!pending) {
                char byte;
                REQUIRE(read(ready[0], &byte, 1) == 1 && byte == 'r');
                pause_ns(30000000);
                REQUIRE(pthread_cancel(thread) == 0);
            }
            REQUIRE(pthread_join(thread, &result) == 0);
            printf("%s %s cancellation: cleanup %d, TLS %d\n", names[operation],
                   pending ? "pending" : "blocking", waiter.cleaned, waiter.tls_ok);
            CHECK(result == PTHREAD_CANCELED && waiter.cleaned == 1 && waiter.tls_ok);
            CHECK(close(fd[0]) == 0 && close(fd[1]) == 0);
            CHECK(close(ready[0]) == 0 && close(ready[1]) == 0);
            if (waiter.ep >= 0)
                CHECK(close(waiter.ep) == 0);
        }
    }
    return finish("blocking and pending cancellation across nine I/O waits with TLS cleanup");
}
