/* SPDX-License-Identifier: LGPL-2.0-or-later */
#ifndef _TCC_TIME_H
#define _TCC_TIME_H
typedef long long time_t;
struct tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year;
    int tm_wday, tm_yday, tm_isdst;
    long tm_gmtoff;
    const char *tm_zone;
};
time_t time(time_t *);
struct tm *localtime(const time_t *);
#endif
