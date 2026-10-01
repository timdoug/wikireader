/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef C33_LTP_SUPERVISED_FORK_H
#define C33_LTP_SUPERVISED_FORK_H
#include <sys/types.h>
pid_t c33_ltp_forbidden_fork(void);
#define fork c33_ltp_forbidden_fork
#endif
