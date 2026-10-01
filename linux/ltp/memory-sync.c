/* C33 resident mappings still need synchronization and error semantics. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
static size_t sync_length;
#define CHECK(condition) do { \
    if (!(condition)) { \
        printf("FAIL line %d: %s (errno %d)\n", __LINE__, #condition, errno); \
        ++failures; \
    } \
} while (0)

static int same_times(const struct stat *a, const struct stat *b)
{
    return a->st_mtim.tv_sec == b->st_mtim.tv_sec &&
           a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
           a->st_ctim.tv_sec == b->st_ctim.tv_sec &&
           a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}

static int changed_times(const struct stat *a, const struct stat *b)
{
    return (a->st_mtim.tv_sec != b->st_mtim.tv_sec ||
            a->st_mtim.tv_nsec != b->st_mtim.tv_nsec) &&
           (a->st_ctim.tv_sec != b->st_ctim.tv_sec ||
            a->st_ctim.tv_nsec != b->st_ctim.tv_nsec);
}

static void *cancel_at_sync(void *address)
{
    if (pthread_cancel(pthread_self()) != 0)
        return (void *)2;
    msync(address, sync_length, MS_SYNC);
    return (void *)1;
}

int main(void)
{
    char name[64], byte;
    long page = sysconf(_SC_PAGESIZE);
    unsigned char *map;
    struct stat before, after;
    pthread_t thread;
    void *result;
    int fd, rc;

    CHECK(page > 0);
    if (page <= 0)
        return 1;
    snprintf(name, sizeof(name), "/c33-sync-%ld", (long)getpid());
    fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    CHECK(fd >= 0);
    if (fd < 0)
        return 1;
    CHECK(shm_unlink(name) == 0);
    CHECK(ftruncate(fd, page) == 0);
    CHECK(pwrite(fd, "a", 1, 0) == 1);
    map = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(map != MAP_FAILED);
    if (map == MAP_FAILED)
        return 1;
    CHECK(fstat(fd, &before) == 0);
    sleep(1);
    map[0] = 'b';
    CHECK(msync(map, page, MS_SYNC) == 0);
    CHECK(fstat(fd, &after) == 0 && changed_times(&before, &after));
    CHECK(pread(fd, &byte, 1, 0) == 1 && byte == 'b');
    before = after;
    sleep(1);
    map[0] = 'c';
    CHECK(msync(map, page, MS_ASYNC) == 0);
    CHECK(fstat(fd, &after) == 0 && changed_times(&before, &after));
    CHECK(pread(fd, &byte, 1, 0) == 1 && byte == 'c');
    errno = 0;
    CHECK(msync(map, page, MS_SYNC | MS_ASYNC) == -1 && errno == EINVAL);
    errno = 0;
    CHECK(msync(map, page, 8) == -1 && errno == EINVAL);
    errno = 0;
    CHECK(msync(map + 1, page, MS_SYNC) == -1 && errno == EINVAL);
    errno = 0;
    CHECK(msync(map, ULONG_MAX, MS_SYNC) == -1 && errno == ENOMEM);
    errno = 0;
    CHECK(msync(NULL, page, MS_SYNC) == -1 && errno == ENOMEM);
    CHECK(msync(map, 0, MS_SYNC) == 0);
    sync_length = page;
    rc = pthread_create(&thread, NULL, cancel_at_sync, map);
    CHECK(rc == 0);
    if (rc == 0)
        CHECK(pthread_join(thread, &result) == 0 && result == PTHREAD_CANCELED);
    CHECK(munmap(map, page) == 0);

    map = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    CHECK(map != MAP_FAILED);
    if (map == MAP_FAILED)
        return 1;
    CHECK(fstat(fd, &before) == 0);
    sleep(1);
    map[0] = 'p';
    CHECK(msync(map, page, MS_SYNC) == 0);
    CHECK(fstat(fd, &after) == 0 && same_times(&before, &after));
    CHECK(pread(fd, &byte, 1, 0) == 1 && byte == 'c');
    CHECK(map[0] == 'p');
    CHECK(munmap(map, page) == 0);

    map = mmap(NULL, page, PROT_READ, MAP_SHARED, fd, 0);
    CHECK(map != MAP_FAILED);
    if (map == MAP_FAILED)
        return 1;
    CHECK(fstat(fd, &before) == 0);
    sleep(1);
    CHECK(msync(map, page, MS_SYNC) == 0);
    CHECK(msync(map, page, MS_ASYNC) == 0);
    CHECK(fstat(fd, &after) == 0 && same_times(&before, &after));
    CHECK(munmap(map, page) == 0);

    /* ramfs allocates contiguous backing pages when grown from zero.
     * Map only pages zero and two: the other pages belong to this file
     * but are absent from our VMA tree. The first mapping is read-only,
     * so its sync cannot hide a skipped timestamp update on page two. */
    CHECK(ftruncate(fd, 0) == 0);
    CHECK(ftruncate(fd, 4 * page) == 0);
    unsigned char *first = mmap(NULL, page, PROT_READ, MAP_SHARED, fd, 0);
    unsigned char *last = mmap(NULL, page, PROT_READ | PROT_WRITE,
                               MAP_SHARED, fd, 2 * page);
    CHECK(first != MAP_FAILED && last != MAP_FAILED);
    if (first == MAP_FAILED || last == MAP_FAILED)
        return 1;
    CHECK((uintptr_t)last == (uintptr_t)first + 2 * page);
    if ((uintptr_t)last != (uintptr_t)first + 2 * page)
        return 1;
    for (int mode = 0; mode < 2; ++mode) {
        for (int gap = 0; gap < 3; ++gap) {
            void *start = gap == 0 ? (void *)((uintptr_t)first + page) :
                          gap == 1 ? first : last;
            size_t length = gap == 1 ? 3 * page : 2 * page;
            CHECK(fstat(fd, &before) == 0);
            sleep(1);
            last[0] = 'd' + mode * 3 + gap;
            errno = 0;
            CHECK(msync(start, length, mode ? MS_ASYNC : MS_SYNC) == -1 &&
                  errno == ENOMEM);
            CHECK(fstat(fd, &after) == 0 && changed_times(&before, &after));
            CHECK(pread(fd, &byte, 1, 2 * page) == 1 &&
                  byte == 'd' + mode * 3 + gap);
        }
    }
    CHECK(munmap(first, page) == 0);
    CHECK(munmap(last, page) == 0);
    CHECK(close(fd) == 0);
    map = mmap(NULL, 3 * page, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(map != MAP_FAILED);
    if (map == MAP_FAILED)
        return 1;
    CHECK(munmap(map + page, page) == 0);
    errno = 0;
    CHECK(msync(map, 3 * page, MS_SYNC) == -1 && errno == ENOMEM);
    CHECK(msync(map, page, MS_SYNC) == 0);
    CHECK(msync(map + 2 * page, page, MS_SYNC) == 0);
    CHECK(munmap(map, page) == 0);
    CHECK(munmap(map + 2 * page, page) == 0);
    if (!failures)
        puts("PASS: shared sync across holes, private/readonly isolation, errors and cancellation");
    return failures ? 1 : 0;
}
