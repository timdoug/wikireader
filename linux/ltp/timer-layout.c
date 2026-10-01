#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

struct guarded_timer {
	unsigned long before;
	struct itimerspec value;
	unsigned long after;
};

static int intact(const struct guarded_timer *p)
{
	return p->before == 0x12345678 && p->after == 0x87654321;
}

int main(void)
{
	struct sigevent event = {.sigev_notify = SIGEV_NONE};
	struct itimerspec setting = {{123, 456789}, {456, 123456789}};
	struct guarded_timer old = {0x12345678, {{0, 0}, {0, 0}}, 0x87654321};
	struct guarded_timer current = old, snapshot;
	timer_t timer;
	int ok = 1;
	if (timer_create(CLOCK_MONOTONIC, &event, &timer)) {
		perror("timer_create");
		return 2;
	}
	if (timer_settime(timer, 0, &setting, &old.value) || !intact(&old) ||
	    old.value.it_interval.tv_sec || old.value.it_interval.tv_nsec ||
	    old.value.it_value.tv_sec || old.value.it_value.tv_nsec)
		ok = 0;
	if (timer_gettime(timer, &current.value) || !intact(&current) ||
	    current.value.it_interval.tv_sec != 123 ||
	    current.value.it_interval.tv_nsec != 456789 ||
	    current.value.it_value.tv_sec < 455 || current.value.it_value.tv_sec > 456)
		ok = 0;
	/* The new value must be copied before an aliased old-value output. */
	current.value.it_interval.tv_sec = 321;
	if (timer_settime(timer, 0, &current.value, &current.value) ||
	    !intact(&current) || current.value.it_interval.tv_sec != 123 ||
	    timer_gettime(timer, &current.value) || !intact(&current) ||
	    current.value.it_interval.tv_sec != 321 ||
	    current.value.it_interval.tv_nsec != 456789)
		ok = 0;
	/* Invalid input must not alter the caller's old-value buffer. */
	snapshot = current;
	setting.it_value.tv_sec = -1;
	errno = 0;
	if (timer_settime(timer, 0, &setting, &current.value) != -1 ||
	    errno != EINVAL || memcmp(&snapshot, &current, sizeof(current)))
		ok = 0;
	if (timer_delete(timer))
		ok = 0;
	puts(ok ? "TIMER LAYOUT PASS" : "TIMER LAYOUT FAIL");
	return ok ? 0 : 1;
}
