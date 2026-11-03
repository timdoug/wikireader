// SPDX-License-Identifier: GPL-2.0-only
/* With 64-bit time_t, libc must convert the kernel's 32-bit timevals for
 * getitimer, getrusage and wait4, and reach clock_adjtime64 for adjtimex. */
#define _GNU_SOURCE
#include <stdio.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/timex.h>
#include <sys/wait.h>
#include <unistd.h>

static int sane(const struct timeval *tv, long long most)
{
	return tv->tv_sec >= 0 && tv->tv_sec <= most &&
	       tv->tv_usec >= 0 && tv->tv_usec < 1000000;
}

int main(void)
{
	volatile unsigned long spin = 0;
	for (unsigned long i = 0; i < 1000000; i++)
		spin = spin + i;

	struct rusage ru;
	if (getrusage(RUSAGE_SELF, &ru) || !sane(&ru.ru_utime, 600) ||
	    !sane(&ru.ru_stime, 600) || ru.ru_utime.tv_sec + ru.ru_utime.tv_usec == 0)
		return 1;

	struct itimerval set = { .it_value = { 100, 0 } }, got;
	if (setitimer(ITIMER_REAL, &set, NULL) || getitimer(ITIMER_REAL, &got) ||
	    !sane(&got.it_value, 100) || got.it_value.tv_sec < 90 ||
	    got.it_interval.tv_sec || got.it_interval.tv_usec)
		return 2;
	set.it_value.tv_sec = 0;
	setitimer(ITIMER_REAL, &set, NULL);

	struct timex tx = { 0 };
	struct timeval now;
	gettimeofday(&now, NULL);
	if (adjtimex(&tx) < 0 || tx.time.tv_sec < now.tv_sec || tx.time.tv_sec > now.tv_sec + 5)
		return 3;

	pid_t pid = vfork();
	if (pid == 0) {
		execl("/bin/true", "true", (char *)NULL);
		_exit(127);
	}
	int status;
	if (wait4(pid, &status, 0, &ru) != pid || !WIFEXITED(status) ||
	    WEXITSTATUS(status) || !sane(&ru.ru_utime, 600) || !sane(&ru.ru_stime, 600))
		return 4;
	puts("TIME64 PASS");
	return 0;
}
