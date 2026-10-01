/* POSIX hints must preserve memory and errno, and validate no-MMU ranges. */
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

static int failures;
#define CHECK(condition) do { \
    if (!(condition)) { \
        printf("FAIL line %d: %s (errno %d)\n", __LINE__, #condition, errno); \
        ++failures; \
    } \
} while (0)

int main(void)
{
    const int advice[] = {POSIX_MADV_NORMAL, POSIX_MADV_RANDOM,
                         POSIX_MADV_SEQUENTIAL, POSIX_MADV_WILLNEED,
                         POSIX_MADV_DONTNEED};
    long page = sysconf(_SC_PAGESIZE);
    unsigned char *buffer;
    size_t i, j;
    CHECK(page > 0);
    if (page <= 0)
        return 1;
    buffer = mmap(NULL, 3 * page, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(buffer != MAP_FAILED);
    if (buffer == MAP_FAILED)
        return 1;
    memset(buffer, 0xa5, 3 * page);
    for (i = 0; i < sizeof(advice) / sizeof(advice[0]); ++i) {
        errno = EDOM;
        CHECK(posix_madvise(buffer, 3 * page, advice[i]) == 0);
        CHECK(errno == EDOM);
        for (j = 0; j < (size_t)(3 * page); ++j)
            CHECK(buffer[j] == 0xa5);
        errno = EDOM;
        CHECK(posix_madvise(buffer, 0, advice[i]) == 0 && errno == EDOM);
        CHECK(posix_madvise(NULL, page, advice[i]) == ENOMEM && errno == EDOM);
        CHECK(posix_madvise(buffer + 1, page, advice[i]) == EINVAL && errno == EDOM);
        CHECK(posix_madvise((void *)((ULONG_MAX / page) * page),
                            2 * page, advice[i]) == EINVAL && errno == EDOM);
        CHECK(posix_madvise(buffer, ULONG_MAX, advice[i]) == EINVAL && errno == EDOM);
    }
    errno = EDOM;
    CHECK(posix_madvise(buffer, page, -1) == EINVAL && errno == EDOM);
    CHECK(posix_madvise(buffer, page, 100) == EINVAL && errno == EDOM);
    /* Raw Linux advice value 4 is destructive, unlike POSIX DONTNEED. */
    errno = 0;
    CHECK(syscall(SYS_madvise, buffer, page, 4) == -1 && errno == EINVAL);
    CHECK(buffer[0] == 0xa5);
    CHECK(munmap(buffer + page, page) == 0);
    errno = EDOM;
    for (i = 0; i < sizeof(advice) / sizeof(advice[0]); ++i) {
        CHECK(posix_madvise(buffer, 3 * page, advice[i]) == ENOMEM && errno == EDOM);
        CHECK(posix_madvise(buffer, page, advice[i]) == 0 && errno == EDOM);
        CHECK(posix_madvise(buffer + 2 * page, page, advice[i]) == 0 && errno == EDOM);
    }
    CHECK(buffer[0] == 0xa5 && buffer[2 * page] == 0xa5);
    CHECK(munmap(buffer, page) == 0);
    CHECK(munmap(buffer + 2 * page, page) == 0);
    if (!failures)
        puts("PASS: POSIX advice preserves contents/errno and validates ranges");
    return failures ? 1 : 0;
}
