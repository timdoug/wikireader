/* No-MMU validation and /dev/zero regressions, separate from LTP counts. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/uio.h>
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
    const int invalid[] = {0, MCL_ONFAULT, 8, -1};
    const int valid[] = {MCL_CURRENT, MCL_FUTURE, MCL_CURRENT | MCL_FUTURE,
                        MCL_CURRENT | MCL_ONFAULT, MCL_FUTURE | MCL_ONFAULT,
                        MCL_CURRENT | MCL_FUTURE | MCL_ONFAULT};
    unsigned char bytes[8193];
    struct iovec vec[] = {{bytes, 4097}, {bytes + 4097, sizeof(bytes) - 4097}};
    long page = sysconf(_SC_PAGESIZE);
    int fd = open("/dev/zero", O_RDONLY);
    void *a, *b;
    size_t i;

    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        errno = 0;
        CHECK(mlockall(invalid[i]) == -1 && errno == EINVAL);
    }
    for (i = 0; i < sizeof(valid) / sizeof(valid[0]); ++i) {
        CHECK(mlockall(valid[i]) == 0);
        CHECK(munlockall() == 0);
    }
    CHECK(page > 0 && fd >= 0);
    if (page <= 0 || fd < 0)
        return 1;
    memset(bytes, 0xa5, sizeof(bytes));
    CHECK(read(fd, bytes, sizeof(bytes)) == sizeof(bytes));
    for (i = 0; i < sizeof(bytes); ++i)
        CHECK(bytes[i] == 0);
    memset(bytes, 0xa5, sizeof(bytes));
    CHECK(readv(fd, vec, 2) == sizeof(bytes));
    for (i = 0; i < sizeof(bytes); ++i)
        CHECK(bytes[i] == 0);
    CHECK(read(fd, bytes, 0) == 0);

    a = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    b = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    CHECK(a != MAP_FAILED && b != MAP_FAILED && a != b);
    if (a != MAP_FAILED && b != MAP_FAILED) {
        for (i = 0; i < (size_t)page; ++i)
            CHECK(((unsigned char *)a)[i] == 0 && ((unsigned char *)b)[i] == 0);
        ((unsigned char *)a)[0] = 0x55;
        CHECK(((unsigned char *)b)[0] == 0);
    }
    if (a != MAP_FAILED) {
        CHECK(munmap(a, page) == 0);
        CHECK(munmap(a, page) == 0);
    }
    if (b != MAP_FAILED)
        CHECK(munmap(b, page) == 0);
    errno = 0;
    CHECK(munmap((void *)1, page) == -1 && errno == EINVAL);
    errno = 0;
    CHECK(munmap((void *)((ULONG_MAX / page) * page), 2 * page) == -1
          && errno == EINVAL);
    errno = 0;
    CHECK(munmap(bytes, 0) == -1 && errno == EINVAL);
    CHECK(close(fd) == 0);
    if (!failures)
        puts("PASS: no-MMU flags, zero reads/mappings, and unmap validation");
    return failures ? 1 : 0;
}
