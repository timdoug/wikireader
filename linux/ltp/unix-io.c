/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "io-checks.h"
#include <fcntl.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

static void stream(void)
{
    int fd[2], pipefd[2], received = -1;
    char first[3] = {0}, rest[3] = {0}, byte = 0;
    struct iovec sendvec[] = {{"ab", 2}, {"cdef", 4}};
    struct iovec recvvec[] = {{first, 1}, {first + 1, 2}};
    union { struct cmsghdr align; char bytes[CMSG_SPACE(sizeof(int))]; } control;
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fd) == 0);
    REQUIRE(pipe(pipefd) == 0);
    CHECK(fcntl(fd[0], F_GETFD) == FD_CLOEXEC);
    CHECK(writev(fd[0], sendvec, 2) == 6);
    CHECK(readv(fd[1], recvvec, 2) == 3 && memcmp(first, "abc", 3) == 0);
    CHECK(read(fd[1], rest, 3) == 3 && memcmp(rest, "def", 3) == 0);

    memset(&control, 0, sizeof(control));
    struct iovec vec = {"r", 1};
    struct msghdr message = {.msg_iov = &vec, .msg_iovlen = 1,
        .msg_control = control.bytes, .msg_controllen = sizeof(control)};
    struct cmsghdr *header = CMSG_FIRSTHDR(&message);
    REQUIRE(header != NULL);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(header), &pipefd[0], sizeof(int));
    CHECK(sendmsg(fd[0], &message, 0) == 1);
    memset(&control, 0, sizeof(control));
    vec.iov_base = &byte;
    message.msg_controllen = sizeof(control);
    CHECK(recvmsg(fd[1], &message, MSG_CMSG_CLOEXEC) == 1 && byte == 'r');
    header = CMSG_FIRSTHDR(&message);
    REQUIRE(header && header->cmsg_level == SOL_SOCKET &&
            header->cmsg_type == SCM_RIGHTS && header->cmsg_len == CMSG_LEN(sizeof(int)));
    memcpy(&received, CMSG_DATA(header), sizeof(int));
    CHECK(fcntl(received, F_GETFD) == FD_CLOEXEC);
    CHECK(close(pipefd[0]) == 0);
    CHECK(write(pipefd[1], "p", 1) == 1);
    CHECK(read(received, &byte, 1) == 1 && byte == 'p');
    CHECK(close(received) == 0 && close(pipefd[1]) == 0);
    CHECK(shutdown(fd[0], SHUT_WR) == 0);
    CHECK(read(fd[1], &byte, 1) == 0);
    CHECK(close(fd[0]) == 0 && close(fd[1]) == 0);
}

struct delayed_send { int fd; int result; };
static void *send_later(void *arg)
{
    struct delayed_send *sender = arg;
    pause_ns(30000000);
    sender->result = send(sender->fd, "t", 1, 0);
    return NULL;
}

static void batches(void)
{
    int fd[2];
    pthread_t thread;
    char bytes[3] = {0};
    struct iovec vec[3];
    struct mmsghdr messages[3] = {0};
    REQUIRE(socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0, fd) == 0);
    for (int n = 0; n < 3; ++n) {
        vec[n].iov_base = &bytes[n];
        vec[n].iov_len = 1;
        messages[n].msg_hdr.msg_iov = &vec[n];
        messages[n].msg_hdr.msg_iovlen = 1;
    }
    for (int wide = 0; wide < 2; ++wide) {
        struct { unsigned long before; struct timespec value; unsigned long after; } timeout =
            {0x12345678, {wide ? 2147483999LL : 0, 250000000}, 0x87654321};
        struct delayed_send sender = {fd[0], -1};
        REQUIRE(pthread_create(&thread, NULL, send_later, &sender) == 0);
        int64_t before = now_ns();
        CHECK(recvmmsg(fd[1], messages, 1, 0, &timeout.value) == 1);
        int64_t elapsed = now_ns() - before;
        REQUIRE(pthread_join(thread, NULL) == 0);
        CHECK(sender.result == 1 && bytes[0] == 't' && messages[0].msg_len == 1);
        CHECK(elapsed >= 25000000 && elapsed < 2000000000);
        CHECK(timeout.before == 0x12345678 && timeout.after == 0x87654321);
        if (!wide)
            CHECK(timeout.value.tv_sec == 0 && timeout.value.tv_nsec >= 0 &&
                  timeout.value.tv_nsec < 240000000);
        else
            CHECK(timeout.value.tv_sec == 2147483999LL &&
                  timeout.value.tv_nsec >= 0 && timeout.value.tv_nsec < 240000000);
    }
    struct timespec timeout = {0, -1}, original = timeout;
    errno = 0;
    CHECK(recvmmsg(fd[1], messages, 1, MSG_DONTWAIT, &timeout) == -1 && errno == EINVAL);
    CHECK(memcmp(&timeout, &original, sizeof(timeout)) == 0);
    timeout.tv_nsec = 1000000;
    original = timeout;
    errno = 0;
    CHECK(recvmmsg(fd[1], messages, 1, MSG_DONTWAIT, &timeout) == -1 && errno == EAGAIN);
    CHECK(memcmp(&timeout, &original, sizeof(timeout)) == 0);
    for (int n = 0; n < 3; ++n)
        CHECK(send(fd[0], "abc" + n, 1, 0) == 1);
    CHECK(recvmmsg(fd[1], messages, 3, MSG_DONTWAIT, NULL) == 3);
    CHECK(memcmp(bytes, "abc", 3) == 0);
    for (int n = 0; n < 3; ++n)
        CHECK(messages[n].msg_len == 1);
    CHECK(close(fd[0]) == 0 && close(fd[1]) == 0);
}

static void backpressure(void)
{
    int fd[2], buffer_size = 2048;
    char buffer[4096] = {0};
    size_t sent = 0, received = 0;
    struct timeval timeout = {0, 30000};
    struct { unsigned long before; struct timeval value; unsigned long after; } output =
        {0x12345678, {-1, -1}, 0x87654321};
    socklen_t length = sizeof(output.value);
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, fd) == 0);
    REQUIRE(setsockopt(fd[0], SOL_SOCKET, SO_SNDBUF, &buffer_size, sizeof(buffer_size)) == 0);
    REQUIRE(setsockopt(fd[0], SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0);
    CHECK(getsockopt(fd[0], SOL_SOCKET, SO_SNDTIMEO, &output.value, &length) == 0);
    CHECK(length == sizeof(output.value) && output.before == 0x12345678 &&
          output.after == 0x87654321 && output.value.tv_sec == 0 &&
          output.value.tv_usec >= 30000 && output.value.tv_usec < 100000);
    for (int n = 0; n < 128; ++n) {
        ssize_t rc = send(fd[0], buffer, sizeof(buffer), MSG_NOSIGNAL);
        if (rc < 0) {
            CHECK(errno == EAGAIN);
            break;
        }
        REQUIRE(rc > 0);
        sent += rc;
    }
    CHECK(sent > 0 && sent < 128 * sizeof(buffer));
    REQUIRE(fcntl(fd[0], F_SETFL, fcntl(fd[0], F_GETFL) & ~O_NONBLOCK) == 0);
    int64_t before = now_ns();
    errno = 0;
    CHECK(send(fd[0], "x", 1, MSG_NOSIGNAL) == -1 && errno == EAGAIN);
    CHECK(now_ns() - before >= 25000000 && now_ns() - before < 2000000000);
    for (;;) {
        ssize_t rc = recv(fd[1], buffer, sizeof(buffer), 0);
        if (rc < 0) {
            CHECK(errno == EAGAIN);
            break;
        }
        REQUIRE(rc > 0);
        received += rc;
    }
    CHECK(received == sent);
    REQUIRE(fcntl(fd[1], F_SETFL, fcntl(fd[1], F_GETFL) & ~O_NONBLOCK) == 0);
    REQUIRE(setsockopt(fd[1], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    length = sizeof(output.value);
    CHECK(getsockopt(fd[1], SOL_SOCKET, SO_RCVTIMEO, &output.value, &length) == 0);
    CHECK(length == sizeof(output.value) && output.before == 0x12345678 &&
          output.after == 0x87654321 && output.value.tv_sec == 0 &&
          output.value.tv_usec >= 30000 && output.value.tv_usec < 100000);
    before = now_ns();
    errno = 0;
    CHECK(recv(fd[1], buffer, sizeof(buffer), 0) == -1 && errno == EAGAIN);
    CHECK(now_ns() - before >= 25000000 && now_ns() - before < 2000000000);
    CHECK(close(fd[0]) == 0 && close(fd[1]) == 0);
}

int main(void)
{
    stream();
    batches();
    backpressure();
    return finish("Unix vectors/EOF/descriptor passing, datagram writeback and socket timeout/backpressure");
}
