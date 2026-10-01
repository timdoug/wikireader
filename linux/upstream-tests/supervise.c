/* Execute a test on no-MMU Linux and reclaim descendants, including those
 * which establish their own process groups in the upstream test skeleton. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t child_pid, timed_out;

static void expired(int number)
{
	(void)number;
	timed_out = 1;
	if (child_pid > 0)
		kill(-child_pid, SIGKILL);
}

static int cleanup(void)
{
	for (int attempt = 0; attempt < 200; ++attempt) {
		DIR *directory = opendir("/proc");
		struct dirent *entry;
		int status, children = 0;
		if (!directory)
			return -1;
		while ((entry = readdir(directory))) {
			char path[64], line[256];
			long pid = strtol(entry->d_name, NULL, 10);
			FILE *file;
			if (pid <= 0)
				continue;
			snprintf(path, sizeof(path), "/proc/%ld/status", pid);
			file = fopen(path, "r");
			if (!file)
				continue;
			while (fgets(line, sizeof(line), file)) {
				if (!strncmp(line, "PPid:", 5) && strtol(line + 5, NULL, 10) == getpid()) {
					kill(pid, SIGKILL);
					children++;
					break;
				}
			}
			fclose(file);
		}
		closedir(directory);
		while (waitpid(-1, &status, WNOHANG) > 0)
			;
		/* An adopted grandchild may appear after its parent is reaped. */
		if (!children && waitpid(-1, &status, WNOHANG) < 0 && errno == ECHILD)
			return 0;
		usleep(10000);
	}
	return -1;
}

int main(int argc, char **argv)
{
	struct sigaction action = {0};
	unsigned int seconds;
	pid_t pid, waited;
	int status;
	if (argc < 3 || !(seconds = strtoul(argv[1], NULL, 10)))
		return 125;
	if (prctl(PR_SET_CHILD_SUBREAPER, 1))
		return 125;
	action.sa_handler = expired;
	sigemptyset(&action.sa_mask);
	if (sigaction(SIGALRM, &action, NULL))
		return 125;
	pid = vfork();
	if (!pid) {
		if (setpgid(0, 0))
			_exit(125);
		execv(argv[2], argv + 2);
		_exit(127);
	}
	if (pid < 0)
		return 125;
	child_pid = pid;
	alarm(seconds);
	do {
		waited = waitpid(pid, &status, 0);
	} while (waited < 0 && errno == EINTR);
	alarm(0);
	kill(-pid, SIGKILL);
	if (cleanup() || waited < 0)
		return 125;
	if (timed_out)
		return 124;
	return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
