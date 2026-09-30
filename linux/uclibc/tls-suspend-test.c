/* Keep the same threads and TLS objects alive across a real s2idle wake. */
#define _GNU_SOURCE
#include <asm/tls.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static __thread volatile unsigned value;
static sem_t ready, wake;
static int errors;

__attribute__((noinline)) static void *tp(void)
{
	__asm__ volatile("" ::: "memory");
	return __builtin_thread_pointer();
}

static int check(void *before, unsigned expected, int saved_errno)
{
	return tp() != before || *(void *volatile *)C33_TLS_SLOT_ADDRESS != before ||
		value != expected || errno != saved_errno;
}

static void *worker(void *unused)
{
	(void)unused;
	value = 0x12345678;
	errno = EDOM;
	void *before = tp();
	if (sem_post(&ready) || sem_wait(&wake)) abort();
	errors = check(before, 0x12345678, EDOM);
	return NULL;
}

int main(void)
{
	int touch = -1;
	for (int i = 0; i < 8; i++) {
		char path[64], name[128] = {0};
		snprintf(path, sizeof(path), "/dev/input/event%d", i);
		int fd = open(path, O_RDONLY);
		if (fd < 0) continue;
		if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0 &&
		    !strcmp(name, "WikiReader touchscreen")) {
			touch = fd;
			break;
		}
		close(fd);
	}
	if (touch < 0 || sem_init(&ready, 0, 0) || sem_init(&wake, 0, 0)) abort();
	pthread_t thread;
	if (pthread_create(&thread, NULL, worker, NULL) || sem_wait(&ready)) abort();
	value = 0xabcdef01;
	void *before = tp();
	puts("TLS SUSPEND READY");
	fflush(stdout);
	errno = ERANGE;
	/* No timer or polling: let the whole machine suspend until the tap. */
	struct input_event event;
	if (read(touch, &event, sizeof(event)) != sizeof(event)) abort();
	int main_errors = check(before, 0xabcdef01, ERANGE);
	if (sem_post(&wake) || pthread_join(thread, NULL)) abort();
	puts(main_errors || errors ? "TLS SUSPEND FAIL" : "TLS SUSPEND PASS");
	return main_errors || errors;
}
