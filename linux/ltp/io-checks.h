/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef C33_IO_CHECKS_H
#define C33_IO_CHECKS_H
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int failures;
#define CHECK(condition) do { \
    if (!(condition)) { \
        printf("FAIL line %d: %s (errno %d)\n", __LINE__, #condition, errno); \
        ++failures; \
    } \
} while (0)
#define REQUIRE(condition) do { \
    if (!(condition)) { \
        printf("SETUP FAIL line %d: %s (errno %d)\n", __LINE__, #condition, errno); \
        exit(1); \
    } \
} while (0)

static inline int64_t now_ns(void)
{
    struct timespec ts;
    REQUIRE(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static inline void pause_ns(long ns)
{
    struct timespec ts = {0, ns};
    while (nanosleep(&ts, &ts) != 0)
        REQUIRE(errno == EINTR);
}

static inline int finish(const char *description)
{
    if (!failures)
        printf("PASS: %s\n", description);
    return failures ? 1 : 0;
}
#endif
