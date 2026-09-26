/* LinuxThreads on C33: contended mutexes (the interrupt-masking
 * testandset), per-thread errno, condition variables, semaphores,
 * pthread_once, thread-specific data and join values. */
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <stdio.h>

#define THREADS 4
#define ROUNDS 4000
#define ITEMS 200

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static volatile long counter;
static int fails;

static void check(int ok, const char *what)
{
	if (!ok) {
		printf("PTHREAD FAIL %s\n", what);
		fails++;
	}
}

static void *adder(void *arg)
{
	long id = (long)arg;

	for (int i = 0; i < ROUNDS; i++) {
		pthread_mutex_lock(&lock);
		long seen = counter;
		/* Widen the window in which an unlocked peer would interleave. */
		if ((i & 255) == 0)
			sched_yield();
		counter = seen + 1;
		pthread_mutex_unlock(&lock);
	}
	errno = 100 + (int)id;
	sched_yield();
	return (void *)(errno == 100 + id ? id * 10 : -1);
}

static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
static int slot, full, produced, consumed_sum;

static void *consumer(void *arg)
{
	(void)arg;
	for (int n = 0; n < ITEMS; n++) {
		pthread_mutex_lock(&lock);
		while (!full)
			pthread_cond_wait(&ready, &lock);
		consumed_sum += slot;
		full = 0;
		pthread_cond_signal(&ready);
		pthread_mutex_unlock(&lock);
	}
	return NULL;
}

static sem_t sem;
static void *poster(void *arg)
{
	(void)arg;
	for (int i = 0; i < 100; i++)
		sem_post(&sem);
	return NULL;
}

static pthread_once_t once = PTHREAD_ONCE_INIT;
static int once_runs;
static void once_fn(void) { once_runs++; }

static pthread_key_t key;
static void *keyed(void *arg)
{
	pthread_once(&once, once_fn);
	pthread_setspecific(key, arg);
	sched_yield();
	return pthread_getspecific(key);
}

int main(void)
{
	pthread_t t[THREADS];

	for (long i = 0; i < THREADS; i++)
		check(pthread_create(&t[i], NULL, adder, (void *)i) == 0, "create");
	errno = 7;
	for (long i = 0; i < THREADS; i++) {
		void *ret;
		check(pthread_join(t[i], &ret) == 0, "join");
		check(ret == (void *)(i * 10), "per-thread errno or join value");
	}
	/* pthread_join may itself leave EINTR; what must not show up here is
	 * any of the values the threads stored in their own errno. */
	check(errno < 100 || errno >= 100 + THREADS, "main thread errno");
	check(counter == THREADS * ROUNDS, "mutex-protected counter");

	pthread_t c;
	check(pthread_create(&c, NULL, consumer, NULL) == 0, "create consumer");
	for (int n = 1; n <= ITEMS; n++) {
		pthread_mutex_lock(&lock);
		while (full)
			pthread_cond_wait(&ready, &lock);
		slot = n;
		full = 1;
		produced += n;
		pthread_cond_signal(&ready);
		pthread_mutex_unlock(&lock);
	}
	pthread_join(c, NULL);
	check(consumed_sum == produced, "condition variable hand-off");

	sem_init(&sem, 0, 0);
	pthread_t p;
	pthread_create(&p, NULL, poster, NULL);
	for (int i = 0; i < 100; i++)
		sem_wait(&sem);
	pthread_join(p, NULL);
	check(1, "semaphore");

	pthread_key_create(&key, NULL);
	pthread_setspecific(key, (void *)0x1234);
	for (long i = 0; i < THREADS; i++)
		pthread_create(&t[i], NULL, keyed, (void *)(i + 1));
	for (long i = 0; i < THREADS; i++) {
		void *ret;
		pthread_join(t[i], &ret);
		check(ret == (void *)(i + 1), "thread-specific data");
	}
	check(pthread_getspecific(key) == (void *)0x1234, "main thread's key");
	check(once_runs == 1, "pthread_once");

	printf("PTHREAD %s: counter %ld, %d failed\n", fails ? "FAIL" : "PASS",
	       counter, fails);
	return fails != 0;
}
