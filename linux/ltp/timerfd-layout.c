/* Kernel time64 nanoseconds are wider than C33's public long fields. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/timerfd.h>
#include <unistd.h>

struct guarded_timer {
    unsigned long before[2];
    struct itimerspec value;
    unsigned long after[4];
};

static int failures;
#define CHECK(condition) do { \
    if (!(condition)) { \
        printf("FAIL line %d: %s (errno %d)\n", __LINE__, #condition, errno); \
        ++failures; \
    } \
} while (0)

static int intact(const struct guarded_timer *p)
{
    return p->before[0] == 0x12345678 && p->before[1] == 0x87654321 &&
           p->after[0] == 0xabcdef01 && p->after[1] == 0x23456789 &&
           p->after[2] == 0x98765432 && p->after[3] == 0x10fedcba;
}

int main(void)
{
    struct guarded_timer current = {
        {0x12345678, 0x87654321}, {{0, 0}, {0, 0}},
        {0xabcdef01, 0x23456789, 0x98765432, 0x10fedcba}
    };
    struct guarded_timer old = current, snapshot;
    struct itimerspec setting = {{123, 456789}, {456, 123456789}};
    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    CHECK(fd >= 0);
    if (fd < 0)
        return 1;

    CHECK(timerfd_gettime(fd, &current.value) == 0 && intact(&current));
    CHECK(current.value.it_interval.tv_sec == 0 &&
          current.value.it_interval.tv_nsec == 0 &&
          current.value.it_value.tv_sec == 0 &&
          current.value.it_value.tv_nsec == 0);
    CHECK(timerfd_settime(fd, 0, &setting, &old.value) == 0 && intact(&old));
    CHECK(old.value.it_interval.tv_sec == 0 && old.value.it_interval.tv_nsec == 0 &&
          old.value.it_value.tv_sec == 0 && old.value.it_value.tv_nsec == 0);
    CHECK(timerfd_gettime(fd, &current.value) == 0 && intact(&current));
    CHECK(current.value.it_interval.tv_sec == 123 &&
          current.value.it_interval.tv_nsec == 456789 &&
          current.value.it_value.tv_sec >= 455 &&
          current.value.it_value.tv_sec <= 456 &&
          current.value.it_value.tv_nsec >= 0 &&
          current.value.it_value.tv_nsec < 1000000000);

    /* Input must be captured before copying an aliased old-value result. */
    current.value.it_interval.tv_sec = 321;
    CHECK(timerfd_settime(fd, 0, &current.value, &current.value) == 0 &&
          intact(&current) && current.value.it_interval.tv_sec == 123);
    CHECK(timerfd_gettime(fd, &current.value) == 0 && intact(&current) &&
          current.value.it_interval.tv_sec == 321 &&
          current.value.it_interval.tv_nsec == 456789);

    /* Exercise all 64 seconds bits without waiting for a distant expiry. */
    setting.it_interval.tv_sec = 2147483999LL;
    setting.it_value.tv_sec = 2147484871LL;
    CHECK(timerfd_settime(fd, 0, &setting, NULL) == 0);
    CHECK(timerfd_gettime(fd, &current.value) == 0 && intact(&current));
    CHECK(current.value.it_interval.tv_sec == setting.it_interval.tv_sec &&
          current.value.it_interval.tv_nsec == setting.it_interval.tv_nsec &&
          current.value.it_value.tv_sec >= setting.it_value.tv_sec - 1 &&
          current.value.it_value.tv_sec <= setting.it_value.tv_sec);
    CHECK(timerfd_settime(fd, 0, &setting, &old.value) == 0 && intact(&old));
    CHECK(old.value.it_interval.tv_sec == setting.it_interval.tv_sec &&
          old.value.it_interval.tv_nsec == setting.it_interval.tv_nsec &&
          old.value.it_value.tv_sec >= setting.it_value.tv_sec - 1 &&
          old.value.it_value.tv_sec <= setting.it_value.tv_sec);

    snapshot = current;
    setting.it_value.tv_sec = -1;
    errno = 0;
    CHECK(timerfd_settime(fd, 0, &setting, &current.value) == -1 &&
          errno == EINVAL && memcmp(&snapshot, &current, sizeof(current)) == 0);
    errno = 0;
    CHECK(timerfd_gettime(-1, &current.value) == -1 && errno == EBADF &&
          memcmp(&snapshot, &current, sizeof(current)) == 0);
    setting.it_value.tv_sec = 1;
    errno = 0;
    CHECK(timerfd_settime(-1, 0, &setting, &current.value) == -1 &&
          errno == EBADF && memcmp(&snapshot, &current, sizeof(current)) == 0);
    CHECK(close(fd) == 0);
    if (!failures)
        puts("PASS: timerfd outputs, aliasing, wide seconds and error isolation");
    return failures ? 1 : 0;
}
