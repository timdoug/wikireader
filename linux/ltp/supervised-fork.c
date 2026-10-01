/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Unexpected helper forks break a trial; they never simulate a child. */
#include <unistd.h>
#include "supervised-fork.h"

pid_t c33_ltp_forbidden_fork(void)
{
    static const char message[] =
        "BROK: unexpected fork in the audited no-MMU harness\n";
    if (write(STDERR_FILENO, message, sizeof(message) - 1) < 0) {
        /* The broken exit status remains authoritative if output fails. */
    }
    _exit(2); /* LTP TBROK */
}
