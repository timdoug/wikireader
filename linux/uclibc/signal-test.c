// SPDX-License-Identifier: GPL-2.0-only
/* Signal ABI and interrupted waits, exercised after a real Grifo boot. */
#define _GNU_SOURCE
#include <errno.h>
#include <linux/futex.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

extern char **environ;
static pid_t child = -1;
static volatile sig_atomic_t caught;
static __thread int tls_value = 33;
static void *thread_pointer;
static volatile sig_atomic_t handler_tls;
static volatile sig_atomic_t handler_alignment;
static unsigned char *alt_memory;
static const size_t alt_size = 32768;
static volatile sig_atomic_t outer_ok, inner_ok;

static void cleanup(void)
{
	if (child > 0) {
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
	}
}

static void require(int ok, const char *what)
{
	if (!ok) {
		fprintf(stderr, "SIGNAL FAIL: %s (errno %d)\n", what, errno);
		exit(1);
	}
}

static int64_t now(void)
{
	struct timespec ts;
	require(clock_gettime(CLOCK_MONOTONIC, &ts) == 0, "monotonic clock");
	return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static void pause_ms(int ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
	while (nanosleep(&ts, &ts) < 0)
		require(errno == EINTR, "controller sleep");
}

extern void c33_signal_handler_entry(int signo);
extern void c33_signal_alt_entry(int signo, siginfo_t *info, void *context);

void c33_signal_handler_body(int signo, unsigned long entry_sp_mod16)
{
	(void)signo;
	caught++;
	handler_alignment = entry_sp_mod16 == 12;
	handler_tls = tls_value == 33 && __builtin_thread_pointer() == thread_pointer;
}

static void install(int flags)
{
	struct sigaction action = {
		.sa_handler = c33_signal_handler_entry, .sa_flags = flags
	};
	sigemptyset(&action.sa_mask);
	caught = handler_tls = handler_alignment = 0;
	require(sigaction(SIGUSR1, &action, NULL) == 0, "install signal handler");
}

struct sender { pthread_t target; int fd; int result; };
static void *send_signal(void *arg)
{
	struct sender *s = arg;
	pause_ms(80);
	s->result = pthread_kill(s->target, SIGUSR1);
	if (s->fd >= 0) {
		pause_ms(80);
		if (write(s->fd, "x", 1) != 1) s->result = -1;
	}
	return NULL;
}

static void interrupted_read(int restart)
{
	int fd[2];
	pthread_t worker;
	require(pipe(fd) == 0, "pipe");
	install(restart ? SA_RESTART : 0);
	struct sender sender = { pthread_self(), fd[1], -1 };
	require(pthread_create(&worker, NULL, send_signal, &sender) == 0, "read sender");
	char byte = 0;
	errno = 0;
	ssize_t result = read(fd[0], &byte, 1);
	int error = errno;
	require(pthread_join(worker, NULL) == 0 && sender.result == 0, "read sender join");
	require(caught == 1 && handler_tls && handler_alignment,
		"read handler TLS / entry alignment");
	require(restart ? result == 1 && byte == 'x' : result == -1 && error == EINTR,
		 restart ? "SA_RESTART read completes" : "read returns EINTR");
	close(fd[0]);
	close(fd[1]);
}

static void interrupted_sleep(int restart)
{
	pthread_t worker;
	install(restart ? SA_RESTART : 0);
	struct sender sender = { pthread_self(), -1, -1 };
	require(pthread_create(&worker, NULL, send_signal, &sender) == 0, "sleep sender");
	struct timespec duration = { 0, 400000000 }, remaining = { -1, -1 };
	int result = nanosleep(&duration, &remaining);
	int error = errno;
	require(pthread_join(worker, NULL) == 0 && sender.result == 0, "sleep sender join");
	require(result == -1 && error == EINTR && caught == 1 && handler_tls &&
		 handler_alignment,
		 "nanosleep returns EINTR even with SA_RESTART");
	require(remaining.tv_sec == 0 && remaining.tv_nsec > 0 &&
		 remaining.tv_nsec < duration.tv_nsec, "nanosleep remaining time64");
}

static void interrupted_userspace(void)
{
	pthread_t worker;
	install(0);
	struct sender sender = { pthread_self(), -1, -1 };
	require(pthread_create(&worker, NULL, send_signal, &sender) == 0, "userspace sender");
	unsigned long a = 0x13579bdf, b = 0x2468ace0, c = 0x12345678;
	errno = 123;
	/* No syscall while waiting: the signal must return to interrupted
	 * userspace instructions, preserving live registers and module GP. */
	while (!caught)
		__asm__ volatile ("" : "+r" (a), "+r" (b), "+r" (c) : : "memory");
	require(a == 0x13579bdf && b == 0x2468ace0 && c == 0x12345678 &&
		 errno == 123 && handler_tls && handler_alignment && tls_value == 33 &&
		 __builtin_thread_pointer() == thread_pointer, "asynchronous userspace signal return");
	require(pthread_join(worker, NULL) == 0 && sender.result == 0 && caught == 1,
		 "userspace sender join");
}

static void interrupted_wait(int restart, int absolute_sleep)
{
	pthread_t worker;
	install(restart ? SA_RESTART : 0);
	struct sender sender = { pthread_self(), -1, -1 };
	require(pthread_create(&worker, NULL, send_signal, &sender) == 0, "wait sender");
	struct timespec deadline = { 0, 400000000 };
	if (absolute_sleep) {
		int64_t end = now() + 400000000;
		deadline.tv_sec = end / 1000000000;
		deadline.tv_nsec = end % 1000000000;
	}
	int result = absolute_sleep ?
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL) :
		ppoll(NULL, 0, &deadline, NULL);
	int error = errno;
	require(pthread_join(worker, NULL) == 0 && sender.result == 0, "wait sender join");
	require(caught == 1 && handler_tls && handler_alignment,
		"wait handler TLS / entry alignment");
	require(absolute_sleep ? result == EINTR : result == -1 && error == EINTR,
		 absolute_sleep ? "absolute clock_nanosleep returns EINTR" :
		 "ppoll returns EINTR even with SA_RESTART");
}

void c33_signal_alt_body(int signo, siginfo_t *info, void *context,
			 unsigned long entry_sp_mod16)
{
	int saved_errno = errno;
	volatile unsigned char marker = 0;
	uintptr_t address = (uintptr_t)&marker;
	int on_stack = address >= (uintptr_t)alt_memory &&
		address < (uintptr_t)alt_memory + alt_size;
	if (signo == SIGUSR2) {
		inner_ok = on_stack && entry_sp_mod16 == 12 && tls_value == 33 &&
			__builtin_thread_pointer() == thread_pointer;
	} else {
		ucontext_t *uc = context;
		sigset_t mask;
		sigprocmask(SIG_SETMASK, NULL, &mask);
		outer_ok = on_stack && entry_sp_mod16 == 12 && info->si_code == SI_QUEUE &&
			info->si_value.sival_int == 73 && info->si_pid == getpid() &&
			sigismember(&uc->uc_sigmask, SIGTERM) == 1 &&
			sigismember(&uc->uc_sigmask, SIGUSR1) == 0 &&
			sigismember(&mask, SIGUSR1) == 1;
		raise(SIGUSR2);
		/* sigreturn must restore the pre-handler mask, including undoing
		 * a mask change made by the handler itself. */
		sigemptyset(&mask);
		sigaddset(&mask, SIGQUIT);
		sigprocmask(SIG_BLOCK, &mask, NULL);
	}
	errno = saved_errno;
}

static void alternate_stack(void)
{
	alt_memory = malloc(alt_size);
	require(alt_memory != NULL, "alternate stack allocation");
	stack_t stack = { .ss_sp = alt_memory, .ss_size = alt_size };
	require(sigaltstack(&stack, NULL) == 0, "install alternate stack");
	struct sigaction action = {
		.sa_sigaction = c33_signal_alt_entry, .sa_flags = SA_ONSTACK | SA_SIGINFO
	};
	sigemptyset(&action.sa_mask);
	require(sigaction(SIGUSR1, &action, NULL) == 0 &&
		 sigaction(SIGUSR2, &action, NULL) == 0, "alternate stack handlers");
	sigset_t mask, saved;
	sigemptyset(&mask);
	sigaddset(&mask, SIGTERM);
	require(sigprocmask(SIG_BLOCK, &mask, &saved) == 0, "pre-handler mask");
	errno = 123;
	union sigval value = { .sival_int = 73 };
	require(sigqueue(getpid(), SIGUSR1, value) == 0, "queued signal");
	require(errno == 123 && outer_ok && inner_ok, "nested alternate stack / siginfo / errno");
	require(sigprocmask(SIG_SETMASK, NULL, &mask) == 0 &&
		 sigismember(&mask, SIGTERM) == 1 && sigismember(&mask, SIGUSR1) == 0 &&
		 sigismember(&mask, SIGQUIT) == 0, "sigreturn restores mask");
	require(sigaltstack(NULL, &stack) == 0 && !(stack.ss_flags & SS_ONSTACK),
		 "sigreturn leaves alternate stack");
	require(sigprocmask(SIG_SETMASK, &saved, NULL) == 0, "restore original mask");
	stack.ss_flags = SS_DISABLE;
	require(sigaltstack(&stack, NULL) == 0, "disable alternate stack");
	free(alt_memory);
}

struct wait_result { int result, error; int64_t elapsed; };
static int stopped_child(const char *kind, int fd)
{
	struct timespec duration = { 0, 600000000 };
	struct wait_result result;
	int64_t start = now();
	require(write(fd, "r", 1) == 1, "child ready");
	if (!strcmp(kind, "nanosleep"))
		result.result = nanosleep(&duration, NULL);
	else if (!strcmp(kind, "clock_nanosleep"))
		result.result = clock_nanosleep(CLOCK_MONOTONIC, 0, &duration, NULL);
	else if (!strcmp(kind, "ppoll"))
		result.result = ppoll(NULL, 0, &duration, NULL);
	else {
		int word = 0;
#ifdef SYS_futex_time64
		struct { int64_t sec, nsec; } timeout = { 0, 600000000 };
		result.result = syscall(SYS_futex_time64, &word, FUTEX_WAIT, 0, &timeout, NULL, 0);
#else
		result.result = syscall(SYS_futex, &word, FUTEX_WAIT, 0, &duration, NULL, 0);
#endif
	}
	result.error = errno;
	result.elapsed = now() - start;
	require(write(fd, &result, sizeof(result)) == sizeof(result), "child result");
	return 0;
}

static void stopped_wait(const char *self, const char *kind)
{
	int fd[2], status;
	require(pipe(fd) == 0, "stop/continue pipe");
	char descriptor[16];
	snprintf(descriptor, sizeof(descriptor), "%d", fd[1]);
	char *args[] = { (char *)self, "--stopped", (char *)kind, descriptor, NULL };
	posix_spawn_file_actions_t actions;
	require(posix_spawn_file_actions_init(&actions) == 0 &&
		 posix_spawn_file_actions_addclose(&actions, fd[0]) == 0, "spawn actions");
	require(posix_spawn(&child, self, &actions, NULL, args, environ) == 0, "spawn stopped child");
	posix_spawn_file_actions_destroy(&actions);
	close(fd[1]);
	char ready;
	require(read(fd[0], &ready, 1) == 1 && ready == 'r', "child wait ready");
	pause_ms(100);
	require(kill(child, SIGSTOP) == 0 && waitpid(child, &status, WUNTRACED) == child &&
		 WIFSTOPPED(status) && WSTOPSIG(status) == SIGSTOP, "child stopped");
	pause_ms(250);
	require(kill(child, SIGCONT) == 0, "child continued");
	struct wait_result result;
	require(read(fd[0], &result, sizeof(result)) == sizeof(result), "read wait result");
	require(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
		 WEXITSTATUS(status) == 0, "stopped child exit");
	child = -1;
	close(fd[0]);
	printf("SIGNAL stop/continue %s: %lld ms\n", kind, (long long)(result.elapsed / 1000000));
	require(!strcmp(kind, "futex") ? result.result == -1 && result.error == ETIMEDOUT :
		result.result == 0, "wait completes without a caught signal");
	if (!strcmp(kind, "ppoll")) {
		/* Linux ppoll uses ERESTARTNOHAND, not a restart block: its
		 * timeout deliberately excludes the 250 ms spent stopped. */
		require(result.elapsed >= 800000000 && result.elapsed < 1050000000,
			 "ppoll keeps Linux stop/continue timeout semantics");
	} else {
		/* 600 ms requested; a 250 ms stop must not add a fresh 600 ms wait.
		 * Allow 200 ms scheduling slack, below the broken ~950 ms. */
		require(result.elapsed >= 550000000 && result.elapsed < 800000000,
			 "stop/continue preserves original deadline");
	}
}

int main(int argc, char **argv)
{
	alarm(15);
	atexit(cleanup);
	if (argc == 4 && !strcmp(argv[1], "--stopped"))
		return stopped_child(argv[2], atoi(argv[3]));
	thread_pointer = __builtin_thread_pointer();
	interrupted_read(0);
	interrupted_read(1);
	interrupted_sleep(0);
	interrupted_sleep(1);
	interrupted_userspace();
	interrupted_wait(0, 0);
	interrupted_wait(1, 0);
	interrupted_wait(0, 1);
	interrupted_wait(1, 1);
	alternate_stack();
	const char *kinds[] = { "nanosleep", "clock_nanosleep", "ppoll", "futex" };
	for (size_t i = 0; i < 4; i++) {
		stopped_wait(argv[0], kinds[i]);
	}
	puts("SIGNAL PASS: entry alignment, EINTR, SA_RESTART, nested alternate stack, siginfo, TLS, stopped deadlines");
	return 0;
}
