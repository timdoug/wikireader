#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

static volatile sig_atomic_t mask_ok;

static void handler(int signal_number)
{
	sigset_t current;
	(void)signal_number;
	mask_ok = !sigprocmask(SIG_SETMASK, NULL, &current) &&
		sigismember(&current, SIGUSR1) == 1 &&
		sigismember(&current, SIGUSR2) == 1 &&
		sigismember(&current, SIGRTMAX) == 1;
}

int main(void)
{
	struct sigaction action = {0}, previous, queried, snapshot;
	int ok;
	action.sa_handler = handler;
	sigemptyset(&action.sa_mask);
	sigaddset(&action.sa_mask, SIGUSR2);
	sigaddset(&action.sa_mask, SIGRTMAX);
	if (sigaction(SIGUSR1, &action, &previous) ||
	    sigaction(SIGUSR1, NULL, &queried)) {
		perror("sigaction");
		return 2;
	}
	ok = queried.sa_handler == handler && queried.sa_flags == action.sa_flags &&
		!memcmp(&queried.sa_mask, &action.sa_mask, sizeof(action.sa_mask));
	if (raise(SIGUSR1) || !mask_ok)
		ok = 0;
	/* Marshal the input before copying the previous action to its buffer. */
	if (sigaction(SIGUSR1, &queried, &queried) ||
	    queried.sa_handler != handler ||
	    memcmp(&queried.sa_mask, &action.sa_mask, sizeof(action.sa_mask)))
		ok = 0;
	snapshot = queried;
	errno = 0;
	if (sigaction(SIGKILL, &action, &queried) != -1 || errno != EINVAL ||
	    memcmp(&queried, &snapshot, sizeof(queried)))
		ok = 0;
	if (sigaction(SIGUSR1, &previous, NULL))
		ok = 0;
	puts(ok ? "SIGNAL MASK PASS" : "SIGNAL MASK FAIL");
	return ok ? 0 : 1;
}
