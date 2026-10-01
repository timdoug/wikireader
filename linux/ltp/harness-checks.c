/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Deliberately failing descriptors validate reporting and supervision. */
#include "tst_test.h"

static void run(void)
{
#if CHECK_MODE == 0
    tst_res(TPASS, "success must be counted");
#elif CHECK_MODE == 1
    tst_res(TFAIL, "failure must remain a failure");
#elif CHECK_MODE == 2
    tst_brk(TBROK, "broken setup must remain broken");
#elif CHECK_MODE == 3
    tst_brk(TCONF, "unsupported case must remain unsupported");
#elif CHECK_MODE == 4
    tst_res(TPASS, "a warning must prevent a clean pass");
    tst_res(TWARN, "intentional warning");
#elif CHECK_MODE == 5
    tst_res(TPASS, "supported subcase");
    tst_res(TCONF, "unsupported subcase must remain visible");
#elif CHECK_MODE == 6
    /* The library must detect a callback with no result. */
#elif CHECK_MODE == 7
    fork(); /* Must break immediately, rather than emulate a child. */
#elif CHECK_MODE == 8
    for (;;)
        pause(); /* The external supervisor must terminate this worker. */
#elif CHECK_MODE == 9
    tst_res(TPASS, "must not run a fork-dependent descriptor");
#endif
}

static struct tst_test test = {
    .test_all = run,
#if CHECK_MODE == 9
    .forks_child = 1,
#endif
};
