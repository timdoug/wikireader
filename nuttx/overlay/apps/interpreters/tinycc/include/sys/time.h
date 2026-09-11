/* SPDX-License-Identifier: LGPL-2.0-or-later */
#ifndef _TCC_SYS_TIME_H
#define _TCC_SYS_TIME_H
#include <time.h>
struct timeval { time_t tv_sec; long tv_usec; };
int gettimeofday(struct timeval *, void *);
#endif
