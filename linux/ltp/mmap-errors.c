/* SPDX-License-Identifier: GPL-2.0-or-later */
/* No-MMU Linux rejects every fixed placement with EINVAL, before anything
   else about the request, and the rejection leaves a live mapping intact. */
#include "io-checks.h"
#include <limits.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static void rejected(void *addr, size_t len, int flags, int error)
{
    void *p;
    errno = 0;
    p = mmap(addr, len, PROT_READ | PROT_WRITE,
             flags | MAP_ANONYMOUS, -1, 0);
    CHECK(p == MAP_FAILED && errno == error);
    if (p != MAP_FAILED)
        CHECK(munmap(p, len) == 0);
}

int main(void)
{
    size_t page = sysconf(_SC_PAGESIZE);
    unsigned char *p;
    unsigned long top = ULONG_MAX & ~(page - 1);
    REQUIRE(page > 0);
    p = mmap(NULL, page, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    REQUIRE(p != MAP_FAILED);
    memset(p, 0x5a, page);

    rejected(p, page, MAP_PRIVATE | MAP_FIXED, EINVAL);
    rejected(p + 1, page, MAP_PRIVATE | MAP_FIXED, EINVAL);
    rejected(p, 0, MAP_PRIVATE | MAP_FIXED, EINVAL);
    rejected(p, page, MAP_FIXED, EINVAL); /* missing mapping type */
    rejected(p, ULONG_MAX, MAP_PRIVATE | MAP_FIXED, EINVAL);
    rejected(p, top, MAP_SHARED | MAP_FIXED, EINVAL);
    rejected((void *)top, page, MAP_PRIVATE | MAP_FIXED, EINVAL);
    rejected((void *)top, 2 * page, MAP_PRIVATE | MAP_FIXED, EINVAL);
    rejected(NULL, page, MAP_PRIVATE | MAP_FIXED, EINVAL);
    /* A hint is not a fixed range, even when hint + length would wrap. */
    {
        void *hint = mmap((void *)top, 2 * page, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        CHECK(hint != MAP_FAILED);
        if (hint != MAP_FAILED)
            CHECK(munmap(hint, 2 * page) == 0);
    }
    CHECK(mlock(p, page) == 0 && munlock(p, page) == 0);
    for (size_t i = 0; i < page; ++i)
        CHECK(p[i] == 0x5a);
    CHECK(munmap(p, page) == 0);
    return finish("fixed placement rejected, ignored hints and mapping preservation");
}
