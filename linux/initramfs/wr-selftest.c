// SPDX-License-Identifier: GPL-2.0
/*
 * The kernel paths the boot's self-tests check from userspace (rcS runs
 * this under wr.selftest): vfork -> execve -> wait4, a signal handler and
 * its return, ptrace stopping a child at every system call, and the C
 * library's own start, stdio and longjmp.  "wr-selftest child" is the child
 * the process and trace tests run.
 */
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHILD_STATUS 23

static volatile sig_atomic_t signal_seen;
static jmp_buf jump_buffer;

static int child(void)
{
	static const char message[] = "C33 child: execve reached the child\n";

	write(1, message, sizeof(message) - 1);
	return CHILD_STATUS;
}

/* Run this program as the child; trace it if asked. */
static pid_t start_child(const char *self, int traced)
{
	char *const argv[] = { (char *)self, "child", NULL };
	pid_t pid = vfork();

	if (pid == 0) {
		if (traced)
			ptrace(PTRACE_TRACEME, 0, 0, 0);
		execv(self, argv);
		_exit(127);
	}
	return pid;
}

static int process_test(const char *self)
{
	pid_t pid = start_child(self, 0);
	int status = 0;

	return pid > 0 && waitpid(pid, &status, 0) == pid &&
	       WIFEXITED(status) && WEXITSTATUS(status) == CHILD_STATUS;
}

/*
 * The syscall path has to report to ptrace, not just run.  A traced child
 * stops twice for every call it makes, so it must reach its exit status
 * through several stops rather than in one step.
 */
static int trace_test(const char *self)
{
	pid_t pid = start_child(self, 1);
	int status = 0, stops = 0;

	if (pid <= 0)
		return 0;
	while (waitpid(pid, &status, 0) == pid && WIFSTOPPED(status) &&
	       stops < 512) {
		stops++;
		unsigned long tls;
		/* The debugger ABI must expose the stopped child's TLS, not ours.
		 * Write the same value back so tracing leaves the child untouched. */
		if (ptrace(PTRACE_GET_THREAD_AREA, pid, 0, &tls) != 0 ||
		    ptrace(PTRACE_SET_THREAD_AREA, pid, 0, &tls) != 0)
			break;
		if (ptrace(PTRACE_SYSCALL, pid, 0, 0) != 0)
			break;
	}
	int passed = stops > 2 && WIFEXITED(status) &&
		     WEXITSTATUS(status) == CHILD_STATUS;
	if (!WIFEXITED(status) && !WIFSIGNALED(status)) {
		kill(pid, SIGKILL);
		waitpid(pid, &status, 0);
	}
	return passed;
}

static void signal_handler(int signal)
{
	signal_seen = signal;
}

static int signal_test(void)
{
	struct sigaction action;

	memset(&action, 0, sizeof(action));
	action.sa_handler = signal_handler;
	signal_seen = 0;
	return sigaction(SIGUSR1, &action, NULL) == 0 &&
	       kill(getpid(), SIGUSR1) == 0 && signal_seen == SIGUSR1;
}

static int libc_test(void)
{
	int jumped = setjmp(jump_buffer);

	if (jumped == 0)
		longjmp(jump_buffer, 7);
	printf("C33 uClibc smoke: pid=%ld longjmp=%d\n", (long)getpid(), jumped);
	fflush(stdout);
	return jumped == 7;
}

static int report(int passed, const char *pass, const char *fail)
{
	puts(passed ? pass : fail);
	fflush(stdout);
	return passed;
}

int main(int argc, char **argv)
{
	int passed = 1;

	if (argc > 1 && strcmp(argv[1], "child") == 0)
		return child();

	passed &= report(process_test(argv[0]),
			 "C33 process test: vfork -> execve -> wait4 passed",
			 "C33 process test FAILED");
	passed &= report(signal_test(),
			 "C33 signal test: handler -> rt_sigreturn passed",
			 "C33 signal test FAILED");
	passed &= report(trace_test(argv[0]),
			 "C33 trace test: PTRACE_SYSCALL stopped the child passed",
			 "C33 trace test FAILED");
	passed &= report(libc_test(),
			 "C33 libc test: crt -> stdio -> getpid -> longjmp passed",
			 "C33 libc test FAILED");
	return passed ? 0 : 1;
}
