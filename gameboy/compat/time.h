/* Peanut-GB names struct tm for its real-time clock setter, which this
   port does not call; mini-libc has no time.h. */
#ifndef WR_GB_TIME_H
#define WR_GB_TIME_H

struct tm {
	int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year;
	int tm_wday, tm_yday, tm_isdst;
};

#endif
