/* SPDX-License-Identifier: GPL-2.0-or-later */
/* A flat address space may return one physical address for several mappings.
 * Every mapping must remain owned until its own unmap or process exit. */
#include "io-checks.h"
#include <fcntl.h>
#include <spawn.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
static size_t page;

static unsigned long number(const char *path, const char *field)
{
    char buf[4096], *p = buf;
    int fd = open(path, O_RDONLY);
    ssize_t n;
    REQUIRE(fd >= 0);
    n = read(fd, buf, sizeof(buf) - 1);
    REQUIRE(n > 0 && close(fd) == 0);
    buf[n] = 0;
    if (field) {
        p = strstr(buf, field);
        REQUIRE(p != NULL);
        p += strlen(field);
    }
    return strtoul(p, NULL, 10);
}

static unsigned long locked(void)
{
    return number("/proc/self/status", "VmLck:");
}

static unsigned int areas(void)
{
    char buf[4096];
    unsigned int count = 0;
    ssize_t n, i;
    int fd = open("/proc/self/maps", O_RDONLY);
    REQUIRE(fd >= 0);
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        for (i = 0; i < n; ++i)
            count += buf[i] == '\n';
    REQUIRE(n == 0 && close(fd) == 0);
    return count;
}

static void set_limit(unsigned long limit)
{
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%lu\n", limit);
    int fd = open("/proc/sys/vm/max_map_count", O_WRONLY);
    REQUIRE(fd >= 0 && write(fd, buf, n) == n && close(fd) == 0);
}

static void *shared(int fd, size_t pages, size_t offset, int prot)
{
    return mmap(NULL, pages * page, prot, MAP_SHARED, fd, offset * page);
}

static void sysv_aliases(void)
{
    struct shmid_ds ds;
    unsigned char *a, *b;
    int id = shmget(IPC_PRIVATE, page, IPC_CREAT | 0600);
    REQUIRE(id >= 0);
    a = shmat(id, NULL, 0);
    b = shmat(id, NULL, 0);
    REQUIRE(a != (void *)-1 && b == a);
    CHECK(shmctl(id, IPC_STAT, &ds) == 0 && ds.shm_nattch == 2);
    a[0] = 0x6e;
    CHECK(mlock(a, page) == 0);
    CHECK(shmdt(b) == 0);
    CHECK(shmctl(id, IPC_STAT, &ds) == 0 && ds.shm_nattch == 1);
    CHECK(mlock(a, page) == 0 && a[0] == 0x6e);
    CHECK(shmdt(a) == 0 && locked() == 0);
    CHECK(shmctl(id, IPC_STAT, &ds) == 0 && ds.shm_nattch == 0);
    CHECK(shmctl(id, IPC_RMID, NULL) == 0);
}

static int child(int id)
{
    int i;
    unsigned char *p, *first = NULL;
    for (i = 0; i < 32; ++i) {
        p = shmat(id, NULL, 0);
        REQUIRE(p != (void *)-1);
        if (!first)
            first = p;
        CHECK(first == p && p[0] == 0x5c);
    }
    /* Deliberately leave all 32 mappings for exit_mmap and vm_ops->close. */
    return failures ? 1 : 0;
}

static void exit_aliases(const char *self)
{
    char text[32];
    char *argv[] = {(char *)self, (char *)"child", text, NULL};
    struct shmid_ds ds;
    int id = shmget(IPC_PRIVATE, page, IPC_CREAT | 0600), status;
    pid_t pid;
    unsigned char *p;
    REQUIRE(id >= 0);
    p = shmat(id, NULL, 0);
    REQUIRE(p != (void *)-1);
    p[0] = 0x5c;
    snprintf(text, sizeof(text), "%d", id);
    REQUIRE(posix_spawn(&pid, self, NULL, NULL, argv, environ) == 0);
    REQUIRE(waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(shmctl(id, IPC_STAT, &ds) == 0 && ds.shm_nattch == 1);
    CHECK(p[0] == 0x5c);
    CHECK(shmdt(p) == 0);
    CHECK(shmctl(id, IPC_STAT, &ds) == 0 && ds.shm_nattch == 0);
    CHECK(shmctl(id, IPC_RMID, NULL) == 0);
}

int main(int argc, char **argv)
{
    char name[64];
    unsigned char *a, *b, *p;
    void *maps[16];
    unsigned long original_limit, before, peak;
    unsigned int base;
    int fd, i;
    page = sysconf(_SC_PAGESIZE);
    REQUIRE(page > 0);
    if (argc == 3 && !strcmp(argv[1], "child"))
        return child(atoi(argv[2]));
    REQUIRE(geteuid() == 0 && munlockall() == 0);
    snprintf(name, sizeof(name), "/c33-mappings-%ld", (long)getpid());
    fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    REQUIRE(fd >= 0 && ftruncate(fd, 3 * page) == 0);
    a = shared(fd, 3, 0, PROT_READ | PROT_WRITE);
    b = shared(fd, 3, 0, PROT_READ | PROT_WRITE);
    REQUIRE(a != MAP_FAILED && b == a);
    a[0] = 0x52;
    CHECK(mlock(a, 3 * page) == 0);
    CHECK(munmap(b, 3 * page) == 0);
    /* This is the pre-fix failure: the previous mapping is no longer in
       the lookup tree after its replacement is unmapped. */
    CHECK(mlock(a, 3 * page) == 0 && locked() == 3 * page / 1024);
    CHECK(msync(a, 3 * page, MS_SYNC) == 0 && a[0] == 0x52);
    CHECK(munmap(a, 3 * page) == 0 && locked() == 0);
    errno = 0;
    CHECK(mlock(a, page) == -1 && errno == ENOMEM);
    if (failures) {
        close(fd);
        shm_unlink(name);
        return finish("no-MMU duplicate mapping ownership");
    }

    /* Nonidentical overlaps cannot share one lookup-tree range. Refuse
       them without clipping or replacing the live mapping. */
    a = shared(fd, 3, 0, PROT_READ | PROT_WRITE);
    REQUIRE(a != MAP_FAILED);
    CHECK(mlock(a, 3 * page) == 0);
    errno = 0;
    CHECK(shared(fd, 1, 1, PROT_READ | PROT_WRITE) == MAP_FAILED && errno == ENOMEM);
    errno = 0;
    CHECK(shared(fd, 2, 0, PROT_READ | PROT_WRITE) == MAP_FAILED && errno == ENOMEM);
    CHECK(mlock(a, 3 * page) == 0 && locked() == 3 * page / 1024);
    CHECK(a[0] == 0x52);
    /* Exact aliases keep their separate mapping metadata and references. */
    b = shared(fd, 3, 0, PROT_READ);
    REQUIRE(b == a);
    CHECK(munmap(b, 3 * page) == 0 && mlock(a, 3 * page) == 0);
    CHECK(munmap(a, 3 * page) == 0 && locked() == 0);

    /* Enforce the existing sysctl for anonymous mappings and aliases.
       Restore it before subprocesses or any potentially allocating I/O. */
    original_limit = number("/proc/sys/vm/max_map_count", NULL);
    base = areas();
    set_limit(base + 4);
    for (i = 0; i < 4; ++i) {
        maps[i] = mmap(NULL, page, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        CHECK(maps[i] != MAP_FAILED);
    }
    errno = 0;
    p = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(p == MAP_FAILED && errno == ENOMEM);
    if (p != MAP_FAILED)
        munmap(p, page);
    for (i = 0; i < 4; ++i)
        if (maps[i] != MAP_FAILED)
            CHECK(munmap(maps[i], page) == 0);
    set_limit(original_limit);

    base = areas();
    set_limit(base + 4);
    a = shared(fd, 3, 0, PROT_READ | PROT_WRITE);
    REQUIRE(a != MAP_FAILED);
    for (i = 0; i < 3; ++i)
        CHECK(shared(fd, 3, 0, PROT_READ | PROT_WRITE) == a);
    errno = 0;
    b = shared(fd, 3, 0, PROT_READ | PROT_WRITE);
    CHECK(b == MAP_FAILED && errno == ENOMEM);
    if (b != MAP_FAILED)
        munmap(b, 3 * page);
    CHECK(munmap(a, 3 * page) == 0); /* reclaim exactly one slot */
    CHECK(shared(fd, 3, 0, PROT_READ | PROT_WRITE) == a);
    CHECK(mlock(a, 3 * page) == 0);
    for (i = 0; i < 4; ++i)
        CHECK(munmap(a, 3 * page) == 0);
    CHECK(locked() == 0);
    set_limit(original_limit);

    a = shared(fd, 3, 0, PROT_READ | PROT_WRITE);
    REQUIRE(a != MAP_FAILED);
    before = number("/proc/self/status", "Mem:");
    for (i = 0; i < 128; ++i)
        REQUIRE(shared(fd, 3, 0, PROT_READ | PROT_WRITE) == a);
    peak = number("/proc/self/status", "Mem:");
    CHECK(peak > before); /* retained metadata must be visible */
    for (i = 0; i < 128; ++i) {
        CHECK(munmap(a, 3 * page) == 0);
        CHECK(mlock(a, 3 * page) == 0);
    }
    CHECK(number("/proc/self/status", "Mem:") == before);
    CHECK(munmap(a, 3 * page) == 0 && locked() == 0);
    CHECK(close(fd) == 0 && shm_unlink(name) == 0);
    sysv_aliases();
    exit_aliases(argv[0]);
    return finish("no-MMU mapping limits, alias ownership, overlaps and System V exit cleanup");
}
