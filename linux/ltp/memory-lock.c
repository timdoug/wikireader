/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Resident no-MMU RAM still has per-mm locking, limits and lifecycle rules. */
#include "io-checks.h"
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <spawn.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
static size_t page;

/* No allocations: querying status must not create a future-locked mapping. */
static unsigned long locked_kb(void)
{
    char buf[4096], *p;
    int fd = open("/proc/self/status", O_RDONLY);
    ssize_t n;
    REQUIRE(fd >= 0);
    n = read(fd, buf, sizeof(buf) - 1);
    REQUIRE(n > 0 && close(fd) == 0);
    buf[n] = '\0';
    p = strstr(buf, "VmLck:");
    REQUIRE(p != NULL);
    return strtoul(p + 6, NULL, 10);
}

static unsigned char *mapping(size_t pages, int flags)
{
    unsigned char *p = mmap(NULL, pages * page, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS | flags, -1, 0);
    REQUIRE(p != MAP_FAILED);
    return p;
}

static void *unlock_thread(void *p)
{
    return (void *)(intptr_t)munlock(p, page);
}

static int child(const char *name)
{
    int fd;
    unsigned char *p, *q;
    struct rlimit original, limit;
    int i;
    CHECK(locked_kb() == 0); /* exec never inherits locks or MCL_FUTURE. */
    fd = shm_open(name, O_RDWR, 0);
    REQUIRE(fd >= 0);
    p = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    REQUIRE(p != MAP_FAILED);
    CHECK(locked_kb() == 0);
    CHECK(p[0] == 0x5a);
    CHECK(munlock(p, page) == 0 && locked_kb() == 0);
    CHECK(msync(p, page, MS_SYNC | MS_INVALIDATE) == 0);
    CHECK(mlock(p, page) == 0 && locked_kb() == page / 1024);
    CHECK(munlockall() == 0 && locked_kb() == 0);
    /* A late limit failure on a reused shared region must release only
       this attempted reference, preserving the parent's live mapping. */
    q = mapping(1, 0);
    REQUIRE(getrlimit(RLIMIT_MEMLOCK, &original) == 0);
    limit = original;
    limit.rlim_cur = page;
    REQUIRE(setrlimit(RLIMIT_MEMLOCK, &limit) == 0 && seteuid(65534) == 0);
    CHECK(mlock(q, page) == 0 && mlockall(MCL_FUTURE) == 0);
    for (i = 0; i < 16; ++i) {
        errno = 0;
        CHECK(mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_SHARED,
                   fd, 0) == MAP_FAILED && errno == EAGAIN);
        CHECK(locked_kb() == page / 1024 && p[0] == 0x5a);
    }
    CHECK(munlockall() == 0);
    REQUIRE(seteuid(0) == 0 && setrlimit(RLIMIT_MEMLOCK, &original) == 0);
    CHECK(munmap(q, page) == 0);
    CHECK(munmap(p, page) == 0 && close(fd) == 0);
    return failures ? 1 : 0;
}

static void process_locks(const char *self)
{
    char name[64];
    char *argv[] = {(char *)self, (char *)"child", name, NULL};
    pid_t pid;
    int fd, status;
    unsigned char *p;
    snprintf(name, sizeof(name), "/c33-lock-%ld", (long)getpid());
    fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    REQUIRE(fd >= 0 && ftruncate(fd, page) == 0);
    p = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    REQUIRE(p != MAP_FAILED);
    p[0] = 0x5a;
    CHECK(mlock(p, page) == 0);
    CHECK(mlockall(MCL_FUTURE) == 0);
    REQUIRE(posix_spawn(&pid, self, NULL, NULL, argv, environ) == 0);
    REQUIRE(waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    /* Another process's unlock cannot clear this process's lock. */
    errno = 0;
    CHECK(msync(p, page, MS_SYNC | MS_INVALIDATE) == -1 && errno == EBUSY);
    CHECK(p[0] == 0x5a);
    CHECK(munlockall() == 0 && locked_kb() == 0);
    CHECK(munmap(p, page) == 0 && close(fd) == 0 && shm_unlink(name) == 0);
}

int main(int argc, char **argv)
{
    unsigned char *p, *q;
    struct rlimit original, limit;
    pthread_t thread;
    void *result;
    unsigned long old_brk, new_brk;
    int i;
    const int bad[] = {0, MCL_ONFAULT, 8, -1};
    const int good[] = {MCL_CURRENT, MCL_FUTURE, MCL_CURRENT | MCL_FUTURE,
        MCL_CURRENT | MCL_ONFAULT, MCL_FUTURE | MCL_ONFAULT,
        MCL_CURRENT | MCL_FUTURE | MCL_ONFAULT};

    page = sysconf(_SC_PAGESIZE);
    REQUIRE(page > 0);
    if (argc == 3 && !strcmp(argv[1], "child"))
        return child(argv[2]);
    REQUIRE(geteuid() == 0);
    CHECK(sysconf(_SC_MEMLOCK) > 0 && sysconf(_SC_MEMLOCK_RANGE) > 0);
    REQUIRE(munlockall() == 0);
    CHECK(locked_kb() == 0);
    p = mapping(3, 0);
    CHECK(mlock(p + 1, page) == 0 && locked_kb() == 2 * page / 1024);
    CHECK(mlock(p, page) == 0 && locked_kb() == 2 * page / 1024);
    errno = 0;
    CHECK(msync(p, page, MS_SYNC | MS_INVALIDATE) == -1 && errno == EBUSY);
    CHECK(msync(p + 2 * page, page, MS_SYNC | MS_INVALIDATE) == 0);
    CHECK(munlock(p + 1, 1) == 0 && locked_kb() == page / 1024);
    CHECK(munlock(p, page) == 0 && locked_kb() == page / 1024);
    CHECK(munmap(p + page, page) == 0 && locked_kb() == 0);
    errno = 0;
    CHECK(mlock(p, 3 * page) == -1 && errno == ENOMEM && locked_kb() == 0);
    CHECK(mlock(p + 2 * page, page) == 0 && locked_kb() == page / 1024);
    CHECK(munmap(p + 2 * page, page) == 0 && locked_kb() == 0);
    CHECK(munmap(p, page) == 0);

    errno = 0;
    CHECK(mlock((void *)(LONG_MAX & ~(page - 1)), 8) == -1 && errno == ENOMEM);
    errno = 0;
    CHECK(munlock((void *)(LONG_MAX & ~(page - 1)), 8) == -1 && errno == ENOMEM);
    errno = 0;
    CHECK(mlock((void *)(ULONG_MAX & ~(page - 1)), 2 * page) == -1 && errno == EINVAL);
    errno = 0;
    CHECK(munlock((void *)1, ULONG_MAX) == -1 && errno == EINVAL);
    CHECK(mlock(NULL, 0) == 0 && munlock(NULL, 0) == 0);

    /* Shrink/regrow must update the lookup tree as well as accounting. */
    p = mapping(3, 0);
    CHECK(mlock(p, 3 * page) == 0 && locked_kb() == 3 * page / 1024);
    REQUIRE(mremap(p, 3 * page, page, 0) == p);
    CHECK(locked_kb() == page / 1024);
    errno = 0;
    CHECK(mlock(p + page, page) == -1 && errno == ENOMEM);
    REQUIRE(mremap(p, page, 3 * page, 0) == p);
    CHECK(locked_kb() == 3 * page / 1024);
    CHECK(mlock(p + 2 * page, page) == 0);
    CHECK(munmap(p, 3 * page) == 0 && locked_kb() == 0);

    /* Private writable file copies can also resize; retain file/VMA
       bookkeeping as well as the anonymous-map tree updates above. */
    {
        char name[64];
        int fd;
        snprintf(name, sizeof(name), "/c33-lock-copy-%ld", (long)getpid());
        fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
        REQUIRE(fd >= 0 && ftruncate(fd, 3 * page) == 0);
        p = mmap(NULL, 3 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
        REQUIRE(p != MAP_FAILED);
        p[2 * page] = 0xa7;
        CHECK(mlock(p, 3 * page) == 0);
        REQUIRE(mremap(p, 3 * page, page, 0) == p);
        CHECK(locked_kb() == page / 1024);
        REQUIRE(mremap(p, page, 3 * page, 0) == p);
        CHECK(locked_kb() == 3 * page / 1024 && p[2 * page] == 0xa7);
        CHECK(munmap(p, 3 * page) == 0 && locked_kb() == 0);
        CHECK(close(fd) == 0 && shm_unlink(name) == 0);
    }

    p = mapping(1, MAP_LOCKED);
    CHECK(locked_kb() == page / 1024);
    CHECK(pthread_create(&thread, NULL, unlock_thread, p) == 0);
    CHECK(pthread_join(thread, &result) == 0 && result == NULL);
    CHECK(locked_kb() == 0);
    CHECK(syscall(SYS_mlock2, p, page, 1) == 0 && locked_kb() == page / 1024);
    errno = 0;
    CHECK(syscall(SYS_mlock2, p, page, 2) == -1 && errno == EINVAL);
    CHECK(munmap(p, page) == 0 && locked_kb() == 0);

    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        errno = 0;
        CHECK(mlockall(bad[i]) == -1 && errno == EINVAL);
    }
    for (i = 0; i < sizeof(good) / sizeof(good[0]); ++i) {
        CHECK(mlockall(good[i]) == 0);
        CHECK(munlockall() == 0 && locked_kb() == 0);
    }
    CHECK(mlockall(MCL_FUTURE) == 0 && locked_kb() == 0);
    p = mapping(1, 0);
    CHECK(locked_kb() == page / 1024);
    CHECK(mlockall(MCL_CURRENT) == 0 && locked_kb() > 0);
    old_brk = locked_kb();
    q = mapping(1, 0);
    CHECK(locked_kb() == old_brk); /* CURRENT replaces the FUTURE policy. */
    CHECK(munlockall() == 0 && locked_kb() == 0);
    CHECK(munmap(p, page) == 0 && munmap(q, page) == 0);
    process_locks(argv[0]);

    /* Keep the hard limit: root can restore credentials and the soft limit. */
    REQUIRE(getrlimit(RLIMIT_MEMLOCK, &original) == 0);
    limit = original;
    limit.rlim_cur = page;
    REQUIRE(setrlimit(RLIMIT_MEMLOCK, &limit) == 0);
    p = mapping(3, 0);
    REQUIRE(seteuid(65534) == 0);
    CHECK(mlock(p, page) == 0 && locked_kb() == page / 1024);
    CHECK(mlock(p + 1, 1) == 0 && locked_kb() == page / 1024);
    errno = 0;
    CHECK(mlock(p + page, page) == -1 && errno == ENOMEM);
    CHECK(locked_kb() == page / 1024);
    REQUIRE(mremap(p, 3 * page, page, 0) == p);
    errno = 0;
    CHECK(mremap(p, page, 3 * page, 0) == MAP_FAILED && errno == EAGAIN);
    CHECK(locked_kb() == page / 1024);
    errno = 0;
    CHECK(mlock(p + page, page) == -1 && errno == ENOMEM);
    CHECK(munlock(p, page) == 0);
    REQUIRE(mremap(p, page, 3 * page, 0) == p);
    CHECK(mlock(p, page) == 0);
    errno = 0;
    CHECK(mlockall(MCL_CURRENT) == -1 && errno == ENOMEM);
    CHECK(locked_kb() == page / 1024);
    CHECK(mlockall(MCL_FUTURE) == 0);
    for (i = 0; i < 8; ++i) {
        errno = 0;
        CHECK(mmap(NULL, page, PROT_READ | PROT_WRITE,
              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED && errno == EAGAIN);
        CHECK(locked_kb() == page / 1024);
    }
    CHECK(munlock(p, page) == 0);
    q = mapping(1, 0);
    CHECK(locked_kb() == page / 1024);
    CHECK(munmap(q, page) == 0 && locked_kb() == 0);
    CHECK(munlockall() == 0);
    REQUIRE(seteuid(0) == 0);
    limit.rlim_cur = 0;
    REQUIRE(setrlimit(RLIMIT_MEMLOCK, &limit) == 0 && seteuid(65534) == 0);
    errno = 0;
    CHECK(mlock(p, page) == -1 && errno == EPERM);
    errno = 0;
    CHECK(mlockall(MCL_CURRENT) == -1 && errno == EPERM);
    errno = 0;
    CHECK(mmap(NULL, page, PROT_READ | PROT_WRITE,
          MAP_PRIVATE | MAP_ANONYMOUS | MAP_LOCKED, -1, 0) == MAP_FAILED && errno == EPERM);
    CHECK(munlock(p, page) == 0 && munlockall() == 0);
    REQUIRE(seteuid(0) == 0);
    CHECK(mlock(p, 3 * page) == 0 && locked_kb() == 3 * page / 1024);
    CHECK(munlockall() == 0);
    REQUIRE(setrlimit(RLIMIT_MEMLOCK, &original) == 0);
    CHECK(munmap(p, 3 * page) == 0);

    /* The ELF-FDPIC loader reserves no brk growth on C33. A rejected
       FUTURE growth must leave both the break and accounting unchanged. */
    CHECK(mlockall(MCL_FUTURE) == 0);
    old_brk = syscall(SYS_brk, 0);
    new_brk = (old_brk + page - 1) & ~(page - 1);
    new_brk += page;
    CHECK(syscall(SYS_brk, new_brk) == old_brk);
    CHECK(locked_kb() == 0 && syscall(SYS_brk, 0) == old_brk);
    CHECK(munlockall() == 0 && locked_kb() == 0);
    return finish("memory-lock validation, limits, accounting, mappings, threads and exec");
}
