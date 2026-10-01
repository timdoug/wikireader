#define _GNU_SOURCE
#include <stdio.h>
#include <time.h>
#include <unistd.h>

int main(void)
{
	struct timespec resolution, now;
	const clockid_t clocks[] = {CLOCK_MONOTONIC, CLOCK_PROCESS_CPUTIME_ID,
				    CLOCK_THREAD_CPUTIME_ID};
	const int options[] = {_SC_MONOTONIC_CLOCK, _SC_CPUTIME, _SC_THREAD_CPUTIME};
	unsigned int i;
	for (i = 0; i < sizeof(clocks) / sizeof(clocks[0]); ++i) {
		long available = sysconf(options[i]);
		if (clock_getres(clocks[i], &resolution) ||
		    clock_gettime(clocks[i], &now) || available <= 0) {
			printf("CLOCK CAPABILITY FAIL: clock=%d sysconf=%ld\n",
			       (int)clocks[i], available);
			return 1;
		}
		printf("CLOCK CAPABILITY: clock=%d sysconf=%ld resolution=%ld ns\n",
		       (int)clocks[i], available, (long)resolution.tv_nsec);
	}
	puts("CLOCK CAPABILITY PASS");
	return 0;
}
