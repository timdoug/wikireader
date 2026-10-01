/* Run an unchanged standalone test in its own process group on no-MMU Linux.
 * The vfork child only establishes its group, execs, or exits on error.
 * The parent enforces a deadline and removes descendants left by the test.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t child_pid;
static volatile sig_atomic_t timed_out;

static void expired(int signal_number)
{
	(void)signal_number;
	timed_out = 1;
	if (child_pid > 0)
		kill(-child_pid, SIGKILL);
}

int main(int argc, char **argv)
{
	struct sigaction action = {0};
	unsigned int seconds;
	pid_t pid, waited;
	int status;

	if (argc < 3 || (seconds = strtoul(argv[1], NULL, 10)) == 0)
		return 125;
	action.sa_handler = expired;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGALRM, &action, NULL))
		return 125;
	if (setenv("LTP_NOMMU_SUPERVISED", "1", 1))
		return 125;

	pid = vfork();
	if (pid == 0) {
		if (setpgid(0, 0))
			_exit(125);
		execv(argv[2], argv + 2);
		_exit(127);
	}
	if (pid < 0) {
		perror("vfork");
		return 125;
	}
	child_pid = pid;
	alarm(seconds);
	do {
		waited = waitpid(pid, &status, 0);
	} while (waited < 0 && errno == EINTR);
	alarm(0);
	kill(-pid, SIGKILL);
	if (waited < 0)
		return 125;
	if (timed_out)
		return 124;
	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	return WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 125;
}
