#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <sys/resource.h>
#include <unistd.h>

static unsigned int errno_entry_alignment;

int *wr_checked_errno_body(unsigned int alignment)
{
	errno_entry_alignment = alignment;
	return __errno_location();
}

int main(void)
{
	struct rlimit limit = {0, 0};
	pid_t pid;
	/* Root bypasses RLIMIT_NPROC. This standalone exec drops its own
	 * credentials so the clone error path is exercised deterministically. */
	if (setrlimit(RLIMIT_NPROC, &limit) || setuid(65534)) {
		perror("prepare vfork failure");
		return 2;
	}
	errno = 0;
	pid = vfork();
	if (pid == 0)
		_exit(99);
	if (pid != -1 || errno != EAGAIN || errno_entry_alignment != 12) {
		printf("VFORK ERROR FAIL: pid=%ld errno=%d errno-helper SP%%16=%u (expected 12)\n",
		       (long)pid, errno, errno_entry_alignment);
		return 1;
	}
	puts("VFORK ERROR PASS: EAGAIN, errno, and C helper entry alignment");
	return 0;
}
