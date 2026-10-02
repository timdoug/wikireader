/* Native ELF TLS and NPTL regression, run through Grifo by app-test.py. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <spawn.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>

#define THREADS 4
#define ROUNDS 40

static __thread int local_exec __attribute__((tls_model("local-exec"))) = 11;
static __thread int initial_exec __attribute__((tls_model("initial-exec"))) = 22;
__thread int global_dynamic __attribute__((tls_model("global-dynamic"))) = 33;
static __thread int local_dynamic __attribute__((tls_model("local-dynamic"))) = 44;
static __thread int zero;
static __thread volatile sig_atomic_t signals;
extern __thread int library_ie __attribute__((tls_model("initial-exec")));
extern int library_tls(int seed);

static pthread_barrier_t barrier;
static pthread_key_t key;
static int destroyed;
static pid_t process;
static int failures;
static pthread_mutex_t robust;
static pthread_mutex_t cancel_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cancel_cond = PTHREAD_COND_INITIALIZER;
static int waiting;
static int cleaned;
static int (*late_tls)(int);
static pthread_spinlock_t spin;
static sem_t notified;
static size_t notified_guard;
static sem_t spin_ready;
static int spin_count;

static void *spin_worker(void *arg)
{
	(void)arg;
	sem_post(&spin_ready);
	for (int i = 0; i < 256; i++) {
		pthread_spin_lock(&spin);
		spin_count++;
		sched_yield(); /* Deliberately give a contender the CPU. */
		pthread_spin_unlock(&spin);
	}
	return NULL;
}

static void check(int yes, const char *what)
{
	if (!yes) {
		printf("NPTL FAIL: %s\n", what);
		failures++;
	}
}

static void destructor(void *p)
{
	if (p)
		__atomic_fetch_add(&destroyed, 1, __ATOMIC_RELAXED);
}

static void caught(int signo)
{
	(void)signo;
	signals++;
}

static void *worker(void *p)
{
	long seed = (long)p;
	int errors = 0;
	pid_t tid = syscall(SYS_gettid);
	void *tp = __builtin_thread_pointer();
	volatile unsigned long *slot = (void *)syscall(__NR_c33_get_tls_slot);
	errors += local_exec != 11 || initial_exec != 22 || global_dynamic != 33 ||
		local_dynamic != 44 || zero != 0 || signals != 0;
	errors += !tp || tp != (void *)*slot ||
		(long)tp != syscall(__NR_c33_get_tls);
	errors += library_tls(seed) != 1 || library_ie != seed;
	errors += pthread_setspecific(key, p) != 0;
	local_exec = seed;
	initial_exec = seed + 1;
	global_dynamic = seed + 2;
	local_dynamic = seed + 3;
	zero = seed + 4;
	errors += pthread_kill(pthread_self(), SIGUSR1) != 0;
	pthread_barrier_wait(&barrier);
	pthread_barrier_wait(&barrier);
	errors += !late_tls || late_tls(seed) != 1;
	errno = 100 + seed;
	for (int i = 0; i < ROUNDS; i++) {
		sched_yield();
		errors += local_exec != seed || initial_exec != seed + 1 ||
			global_dynamic != seed + 2 || local_dynamic != seed + 3 ||
			zero != seed + 4 || signals != 1 || errno != 100 + seed;
		errors += getpid() != process || syscall(SYS_gettid) != tid;
		errors += __builtin_thread_pointer() != tp || (void *)*slot != tp;
		errors += library_tls(-1) != seed * 3 + 3 ||
			pthread_getspecific(key) != p;
		errors += !late_tls || late_tls(-1) != seed;
		errno = 100 + seed;
	}
	return (void *)(long)errors;
}

static void *owner(void *arg)
{
	(void)arg;
	return (void *)(long)pthread_mutex_lock(&robust);
}

static void cleanup(void *arg)
{
	(void)arg;
	cleaned++;
	pthread_mutex_unlock(&cancel_lock);
}

static void *returns(void *arg)
{
	return arg;
}

static void notify(union sigval value)
{
	pthread_attr_t attr;
	(void)value;
	if (pthread_getattr_np(pthread_self(), &attr) == 0) {
		pthread_attr_getguardsize(&attr, &notified_guard);
		pthread_attr_destroy(&attr);
	}
	sem_post(&notified);
}

static void *cancelled(void *arg)
{
	(void)arg;
	pthread_mutex_lock(&cancel_lock);
	pthread_cleanup_push(cleanup, NULL);
	waiting = 1;
	pthread_cond_signal(&cancel_cond);
	for (;;)
		pthread_cond_wait(&cancel_cond, &cancel_lock);
	pthread_cleanup_pop(1);
	return NULL;
}

int main(void)
{
	pthread_t threads[THREADS];
	process = getpid();
	struct sigaction action = { .sa_handler = caught };
	sigemptyset(&action.sa_mask);
	check(sigaction(SIGUSR1, &action, NULL) == 0, "signal handler");
	check(pthread_barrier_init(&barrier, NULL, THREADS + 1) == 0, "barrier init");
	check(pthread_key_create(&key, destructor) == 0, "key init");
	for (long i = 0; i < THREADS; i++)
		check(pthread_create(&threads[i], NULL, worker, (void *)(i + 1)) == 0,
		      "create TLS worker");
	pthread_barrier_wait(&barrier);
	void *late = dlopen("/mnt/sd/tlslate.so", RTLD_NOW | RTLD_LOCAL);
	check(late != NULL, "dlopen TLS after threads start");
	if (late)
		late_tls = dlsym(late, "late_tls");
	check(late_tls && late_tls(100) == 1, "main dynamic TLS alignment/init");
	pthread_barrier_wait(&barrier);
	for (int i = 0; i < THREADS; i++) {
		void *result = (void *)-1;
		check(pthread_join(threads[i], &result) == 0, "join TLS worker");
		check(result == NULL, "thread identity / TLS isolation / signals");
	}
	check(destroyed == THREADS, "TSD destructors");
	check(late_tls && late_tls(-1) == 100, "main dynamic TLS isolation");
	if (late)
		check(dlclose(late) == 0, "dlclose TLS");
	check(local_exec == 11 && initial_exec == 22 && global_dynamic == 33 &&
	      local_dynamic == 44 && zero == 0 && signals == 0 &&
	      library_ie == 71 && library_tls(-1) == 216, "main TLS untouched");

	pthread_mutexattr_t attr;
	check(pthread_mutexattr_init(&attr) == 0, "mutex attr");
	check(pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST) == 0, "robust attr");
	check(pthread_mutex_init(&robust, &attr) == 0, "robust init");
	check(pthread_create(&threads[0], NULL, owner, NULL) == 0, "robust owner");
	void *result;
	check(pthread_join(threads[0], &result) == 0 && result == NULL, "owner exit");
	check(pthread_mutex_lock(&robust) == EOWNERDEAD, "owner death");
	check(pthread_mutex_consistent(&robust) == 0, "owner recovery");
	check(pthread_mutex_unlock(&robust) == 0, "robust unlock");
	check(pthread_mutex_destroy(&robust) == 0, "robust destroy");
	pthread_mutexattr_destroy(&attr);

	check(pthread_create(&threads[0], NULL, cancelled, NULL) == 0, "cancel worker");
	pthread_mutex_lock(&cancel_lock);
	while (!waiting)
		pthread_cond_wait(&cancel_cond, &cancel_lock);
	pthread_mutex_unlock(&cancel_lock);
	check(pthread_cancel(threads[0]) == 0, "cancel request");
	check(pthread_join(threads[0], &result) == 0 && result == PTHREAD_CANCELED,
	      "cancel join");
	check(cleaned == 1 && pthread_mutex_trylock(&cancel_lock) == 0,
	      "cancel cleanup unlock");
	pthread_mutex_unlock(&cancel_lock);
	check(pthread_spin_init(&spin, PTHREAD_PROCESS_PRIVATE) == 0, "spin init");
	check(sem_init(&spin_ready, 0, 0) == 0, "spin ready init");
	check(pthread_spin_lock(&spin) == 0, "spin lock");
	check(pthread_spin_trylock(&spin) == EBUSY, "spin trylock held");
	check(pthread_create(&threads[0], NULL, spin_worker, NULL) == 0, "spin worker");
	sem_wait(&spin_ready);
	sched_yield();
	pthread_spin_unlock(&spin);
	for (int i = 0; i < 256; i++) {
		pthread_spin_lock(&spin);
		spin_count++;
		sched_yield();
		pthread_spin_unlock(&spin);
	}
	check(pthread_join(threads[0], NULL) == 0 && spin_count == 512, "contended spinlock");
	check(pthread_spin_trylock(&spin) == 0, "spin trylock free");
	pthread_spin_unlock(&spin);
	pthread_spin_destroy(&spin);
	sem_destroy(&spin_ready);

	/* No fork() can run handlers without an MMU; registering one still
	 * succeeds. A guard as large as the stack is not carved from it. */
	check(pthread_atfork(NULL, NULL, NULL) == 0, "atfork registration");
	pthread_attr_t guard;
	size_t stack = 0;
	check(pthread_attr_init(&guard) == 0 &&
	      pthread_attr_getstacksize(&guard, &stack) == 0 &&
	      pthread_attr_setguardsize(&guard, stack) == 0, "guard attributes");
	check(pthread_create(&threads[0], &guard, returns, &stack) == 0 &&
	      pthread_join(threads[0], &result) == 0 && result == &stack,
	      "guard as large as the stack");
	/* mq_notify() starts SIGEV_THREAD notifications with those attributes. */
	struct mq_attr queue_attr = { .mq_maxmsg = 1, .mq_msgsize = 1 };
	mq_unlink("/nptl-test");
	mqd_t queue = mq_open("/nptl-test", O_CREAT | O_EXCL | O_RDWR, 0600, &queue_attr);
	struct sigevent event = { .sigev_notify = SIGEV_THREAD,
				  .sigev_notify_function = notify,
				  .sigev_notify_attributes = &guard };
	check(sem_init(&notified, 0, 0) == 0 && queue != (mqd_t)-1 &&
	      mq_notify(queue, &event) == 0, "mq_notify SIGEV_THREAD");
	struct timespec deadline;
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += 10;
	check(mq_send(queue, "", 1, 0) == 0 && sem_timedwait(&notified, &deadline) == 0 &&
	      notified_guard == stack, "notification thread and its guard size");
	mq_close(queue);
	mq_unlink("/nptl-test");
	sem_destroy(&notified);
	pthread_attr_destroy(&guard);
	char *args[] = { "tlsexec", NULL };
	extern char **environ;
	pid_t child;
	int status;
	int error = posix_spawn(&child, "/mnt/sd/tlsexec.bin", NULL, NULL, args, environ);
	check(error == 0, "exec TLS probe spawn");
	if (!error)
		check(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
		      WEXITSTATUS(status) == 0, "exec clears TP before libc starts");

	printf("NPTL %s: native ELF TLS, dlopen, identity, signals, destructors, robust mutex, cancellation, mq_notify\n",
	       failures ? "FAIL" : "PASS");
	return failures != 0;
}
