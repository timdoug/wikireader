/* Diagnostic timings, run through Grifo by runtime-bench.py. */
#define _GNU_SOURCE
#include <pthread.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>

extern char **environ;
extern const char wr_mutex_begin[], wr_mutex_end[];
extern const char wr_exec_begin[], wr_exec_end[];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static void *worker(void *arg) { return arg; }
static long long now(void)
{
	struct timespec t;
	if (clock_gettime(CLOCK_MONOTONIC, &t)) abort();
	return (long long)t.tv_sec * 1000000000 + t.tv_nsec;
}
int main(int argc, char **argv)
{
	int single = argc == 2 && !strcmp(argv[1], "--single");
	if (argc > 1 && !single) {
		fputs("Usage: runtime-bench [--single]\n", stderr);
		return 2;
	}
	pthread_t thread;
	if (!single &&
	    (pthread_create(&thread, NULL, worker, NULL) || pthread_join(thread, NULL))) abort();
	printf("BENCH mode: %s\n", single ? "single-threaded" : "after pthread_create");
	printf("BENCH PC window: %p %p\n", wr_mutex_begin, wr_mutex_end);
	printf("BENCH EXEC PC window: %p %p\n", wr_exec_begin, wr_exec_end);
	/* PID 1 supplies the shared BusyBox text mapping for spawned children. */
	const char *mapfiles[] = { "/proc/self/maps", "/proc/1/maps" };
	for (int i = 0; i < 2; i++) {
		FILE *maps = fopen(mapfiles[i], "r");
		if (!maps) abort();
		char line[256];
		while (fgets(line, sizeof(line), maps)) printf("BENCH MAP %s", line);
		fclose(maps);
	}
	long long start = now();
	__asm__ volatile(".global wr_mutex_begin\nwr_mutex_begin:" ::: "memory");
	for (int i = 0; i < 10000; i++) {
		if (pthread_mutex_lock(&lock) || pthread_mutex_unlock(&lock)) abort();
	}
	__asm__ volatile(".global wr_mutex_end\nwr_mutex_end:" ::: "memory");
	printf("BENCH mutex lock/unlock: %lld ns\n", (now() - start) / 10000);
	start = now();
	__asm__ volatile(".global wr_exec_begin\nwr_exec_begin: nop" ::: "memory");
	for (int i = 0; i < 32; i++) {
		pid_t pid;
		char *args[] = { "true", NULL };
		int status;
		if (posix_spawn(&pid, "/bin/true", NULL, NULL, args, environ) ||
		    waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status)) abort();
	}
	__asm__ volatile(".global wr_exec_end\nwr_exec_end: nop" ::: "memory");
	printf("BENCH spawn/exec/wait /bin/true: %lld ns\n", (now() - start) / 32);
	puts("BENCH PASS");
	return 0;
}
