// SPDX-License-Identifier: GPL-2.0-only
/*
 * Run test programs from an archive, one at a time, and record how each
 * ended and what it printed.
 *
 *   runner ARCHIVE RESULTS SECONDS
 *
 * ARCHIVE is a series of "NAME SIZE\n" headers each followed by SIZE bytes
 * of program.  Each program is written to /tmp, run with stdin from
 * /dev/null and stdout and stderr to a file, and killed after SECONDS.
 * RESULTS gets, for each, "NAME STATUS LENGTH\n" and LENGTH bytes of its
 * output, STATUS being exit-N, signal-N or timeout.  It is flushed to the
 * card after every program, so a run cut short keeps what it did.
 *
 * Without an MMU a program's memory is contiguous, and the page cache a
 * long run builds up breaks free memory into pieces too small for GCC's
 * larger tests, which take a failed malloc's NULL and write through it,
 * over the kernel.  So the cache is dropped before each program.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define PROGRAM	"/tmp/gcc-test.bin"
#define OUTPUT	"/tmp/gcc-test.out"
#define MAX_OUTPUT	65536

static volatile sig_atomic_t timed_out;

static void on_alarm(int sig)
{
	(void)sig;
	timed_out = 1;
}

static int copy_program(FILE *archive, long size)
{
	char buf[4096];
	int fd = open(PROGRAM, O_WRONLY | O_CREAT | O_TRUNC, 0755);

	if (fd < 0)
		return -1;
	while (size > 0) {
		size_t n = fread(buf, 1, size < (long)sizeof(buf) ? size : sizeof(buf), archive);

		if (n == 0 || write(fd, buf, n) != (ssize_t)n) {
			close(fd);
			return -1;
		}
		size -= n;
	}
	return close(fd);
}

static int drop_caches(void)
{
	int fd = open("/proc/sys/vm/drop_caches", O_WRONLY);
	int ok;

	if (fd < 0)
		return -1;
	ok = write(fd, "3\n", 2) == 2;
	close(fd);
	return ok ? 0 : -1;
}

static void run(const char *name, FILE *results, unsigned int seconds)
{
	char status[32], buf[4096];
	struct sigaction sa;
	int wstatus, fd;
	long length = 0;
	pid_t pid;

	drop_caches();
	pid = vfork();
	if (pid == 0) {
		int in = open("/dev/null", O_RDONLY);
		int out = open(OUTPUT, O_WRONLY | O_CREAT | O_TRUNC, 0644);

		dup2(in, 0);
		dup2(out, 1);
		dup2(out, 2);
		execl(PROGRAM, PROGRAM, (char *)NULL);
		_exit(127);
	}
	if (pid < 0) {
		snprintf(status, sizeof(status), "vfork-%d", errno);
	} else {
		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = on_alarm;	/* no SA_RESTART: waitpid returns */
		sigaction(SIGALRM, &sa, NULL);
		timed_out = 0;
		alarm(seconds);
		while (waitpid(pid, &wstatus, 0) < 0) {
			if (errno == EINTR && timed_out) {
				kill(pid, SIGKILL);
				timed_out = 2;
			} else if (errno != EINTR) {
				break;
			}
		}
		alarm(0);
		if (timed_out == 2)
			snprintf(status, sizeof(status), "timeout");
		else if (WIFEXITED(wstatus))
			snprintf(status, sizeof(status), "exit-%d", WEXITSTATUS(wstatus));
		else
			snprintf(status, sizeof(status), "signal-%d", WTERMSIG(wstatus));
	}

	fd = open(OUTPUT, O_RDONLY);
	if (fd >= 0) {
		length = lseek(fd, 0, SEEK_END);
		if (length > MAX_OUTPUT)
			length = MAX_OUTPUT;
		lseek(fd, 0, SEEK_SET);
	}
	fprintf(results, "%s %s %ld\n", name, status, length);
	while (fd >= 0 && length > 0) {
		ssize_t n = read(fd, buf, length < (long)sizeof(buf) ? length : sizeof(buf));

		if (n <= 0)
			break;
		fwrite(buf, 1, n, results);
		length -= n;
	}
	if (fd >= 0)
		close(fd);
	fputc('\n', results);
	fflush(results);
	fsync(fileno(results));
}

int main(int argc, char **argv)
{
	char line[256], name[128];
	unsigned int seconds;
	FILE *archive, *results;
	long size;
	int count = 0;

	if (argc != 4)
		return 2;
	archive = fopen(argv[1], "r");
	results = fopen(argv[2], "w");
	seconds = atoi(argv[3]);
	if (!archive || !results)
		return 2;
	if (drop_caches() < 0) {
		fprintf(stderr, "runner: cannot drop caches: %s\n", strerror(errno));
		return 1;
	}
	printf("GCCTEST START\n");
	fflush(stdout);
	while (fgets(line, sizeof(line), archive)) {
		if (sscanf(line, "%127s %ld", name, &size) != 2)
			break;
		if (copy_program(archive, size) < 0) {
			fprintf(stderr, "runner: cannot write %s\n", name);
			return 1;
		}
		run(name, results, seconds);
		if (++count % 100 == 0)
			printf("GCCTEST %d run\n", count);
	}
	fclose(results);
	printf("GCCTEST DONE %d\n", count);
	return 0;
}
