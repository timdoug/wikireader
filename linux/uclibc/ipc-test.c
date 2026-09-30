// SPDX-License-Identifier: GPL-2.0-only
/* Separate exec'd processes exchange a page using POSIX and System V IPC.
 * No fork, inherited pointers, process-private mutexes or busy waiting. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <spawn.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <asm/shmbuf.h>
#include <asm/sembuf.h>

/* The libc's kernel-facing prefix must match the generic C33 UAPI. */
_Static_assert(offsetof(struct shmid_ds, shm_cpid) == offsetof(struct shmid64_ds, shm_cpid), "shm time64 layout");
_Static_assert(offsetof(struct semid_ds, sem_nsems) == offsetof(struct semid64_ds, sem_nsems), "sem time64 layout");

#define ROUNDS 64
struct page { uint32_t sequence; pid_t writer; unsigned char data[4088]; };
union semun { int val; struct semid_ds *buf; unsigned short *array; };
static int shmid = -1, semid = -1, msgid = -1;
static char shm_name[40], request_name[40], reply_name[40];
static pid_t child_pid = -1;
extern char **environ;

static void cleanup(void)
{
	if (child_pid > 0) {
		kill(child_pid, SIGKILL);
		waitpid(child_pid, NULL, 0);
	}
	if (*shm_name) shm_unlink(shm_name);
	if (*request_name) mq_unlink(request_name);
	if (*reply_name) mq_unlink(reply_name);
	if (shmid >= 0) shmctl(shmid, IPC_RMID, NULL);
	if (semid >= 0) semctl(semid, 0, IPC_RMID);
	if (msgid >= 0) msgctl(msgid, IPC_RMID, NULL);
}

static void require(int ok, const char *what)
{
	if (!ok) {
		fprintf(stderr, "IPC FAIL %s: errno %d (%s)\n", what, errno, strerror(errno));
		exit(1);
	}
}

static struct timespec deadline(void)
{
	struct timespec ts;
	require(clock_gettime(CLOCK_REALTIME, &ts) == 0, "clock");
	ts.tv_sec += 5;
	return ts;
}

static void send_token(mqd_t queue, uint32_t token)
{
	struct timespec ts = deadline();
	require(mq_timedsend(queue, (char *)&token, sizeof(token), 0, &ts) == 0, "mq_timedsend");
}

static void receive_token(mqd_t queue, uint32_t token)
{
	struct timespec ts = deadline();
	uint32_t seen = 0;
	require(mq_timedreceive(queue, (char *)&seen, sizeof(seen), NULL, &ts) == sizeof(seen), "mq_timedreceive");
	require(seen == token, "queue sequence");
}

static void semaphore(int id, unsigned short index, short delta)
{
	struct sembuf op = { index, delta, 0 };
	struct timespec ts = { 5, 0 };
	require(semtimedop(id, &op, 1, &ts) == 0, "semtimedop");
}

static void fill(struct page *p, uint32_t sequence, int reply)
{
	p->sequence = sequence;
	p->writer = getpid();
	for (unsigned i = 0; i < sizeof(p->data); i++)
		p->data[i] = (unsigned char)(sequence + i * 13) ^ (reply ? 0xff : 0);
}

static void verify(struct page *p, uint32_t sequence, int reply)
{
	require(p->sequence == sequence && p->writer != getpid(), "separate process wrote page");
	for (unsigned i = 0; i < sizeof(p->data); i++)
		require(p->data[i] == ((unsigned char)(sequence + i * 13) ^ (reply ? 0xff : 0)), "shared payload");
}

static int child(int argc, char **argv)
{
	require(argc == 5, "child arguments");
	int sysv = strcmp(argv[1], "sysv-child") == 0;
	struct page *p;
	mqd_t request = -1, reply = -1;
	int sem = -1;
	if (sysv) {
		p = shmat(atoi(argv[2]), NULL, 0);
		require(p != (void *)-1, "child shmat");
		sem = atoi(argv[3]);
	} else {
		int fd = shm_open(argv[2], O_RDWR, 0);
		require(fd >= 0, "child shm_open");
		p = mmap(NULL, sizeof(*p), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		require(p != MAP_FAILED, "child shared mmap");
		close(fd);
		request = mq_open(argv[3], O_RDONLY);
		reply = mq_open(argv[4], O_WRONLY);
		require(request != -1 && reply != -1, "child mq_open");
	}
	for (uint32_t n = 1; n <= ROUNDS; n++) {
		if (sysv) semaphore(sem, 0, -1); else receive_token(request, n);
		verify(p, n, 0);
		fill(p, n, 1);
		if (sysv) semaphore(sem, 1, 1); else send_token(reply, n);
	}
	if (sysv) require(shmdt(p) == 0, "child shmdt");
	else {
		require(munmap(p, sizeof(*p)) == 0, "child munmap");
		mq_close(request);
		mq_close(reply);
	}
	return 0;
}

static void exchange(const char *program, int sysv)
{
	struct page *p;
	mqd_t request = -1, reply = -1;
	char id[24], sem[24];
	if (sysv) {
		shmid = shmget(IPC_PRIVATE, sizeof(*p), IPC_CREAT | 0600);
		require(shmid >= 0, "shmget");
		p = shmat(shmid, NULL, 0);
		require(p != (void *)-1, "parent shmat");
		struct shmid_ds info;
		require(shmctl(shmid, IPC_STAT, &info) == 0, "shmctl IPC_STAT");
		require(info.shm_segsz == sizeof(*p) && info.shm_cpid == getpid() && info.shm_nattch == 1, "shm statistics layout");
		require(info.shm_atime > 0 && info.shm_ctime > 0, "shm time64 statistics");
		semid = semget(IPC_PRIVATE, 2, IPC_CREAT | 0600);
		require(semid >= 0, "semget");
		require(semctl(semid, 0, SETVAL, (union semun){ .val = 7 }) == 0, "semctl integer argument");
		require(semctl(semid, 0, GETVAL) == 7 && semctl(semid, 0, GETPID) == getpid(), "semctl value and PID");
		unsigned short values[2] = { 0, 0 };
		require(semctl(semid, 0, SETALL, (union semun){ .array = values }) == 0, "semctl SETALL");
		struct semid_ds si;
		require(semctl(semid, 0, IPC_STAT, (union semun){ .buf = &si }) == 0, "semctl IPC_STAT");
		require(si.sem_nsems == 2 && si.sem_ctime > 0, "sem statistics layout/time64");
		require(semctl(semid, 0, GETVAL) == 0, "three-argument semctl");
		snprintf(id, sizeof(id), "%d", shmid);
		snprintf(sem, sizeof(sem), "%d", semid);
	} else {
		int fd = shm_open(shm_name, O_CREAT | O_EXCL | O_RDWR, 0600);
		require(fd >= 0, "parent shm_open");
		require(fcntl(fd, F_GETFD) & FD_CLOEXEC, "shm close-on-exec");
		require(ftruncate(fd, sizeof(*p)) == 0, "shm ftruncate");
		p = mmap(NULL, sizeof(*p), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		require(p != MAP_FAILED, "parent shared mmap");
		close(fd);
		struct mq_attr attr = { .mq_maxmsg = 4, .mq_msgsize = sizeof(uint32_t) };
		request = mq_open(request_name, O_CREAT | O_EXCL | O_RDWR, 0600, &attr);
		reply = mq_open(reply_name, O_CREAT | O_EXCL | O_RDWR, 0600, &attr);
		require(request != -1 && reply != -1, "parent mq_open");
	}
	for (unsigned i = 0; i < sizeof(*p); i++)
		require(((unsigned char *)p)[i] == 0, "new shared memory is zeroed");
	char *args[] = { (char *)program, sysv ? "sysv-child" : "posix-child",
		sysv ? id : shm_name, sysv ? sem : request_name, reply_name, NULL };
	int error = posix_spawn(&child_pid, program, NULL, NULL, args, environ);
	if (error) errno = error;
	require(error == 0, "posix_spawn independent process");
	for (uint32_t n = 1; n <= ROUNDS; n++) {
		fill(p, n, 0);
		if (sysv) semaphore(semid, 0, 1); else send_token(request, n);
		if (sysv) semaphore(semid, 1, -1); else receive_token(reply, n);
		verify(p, n, 1);
	}
	int status;
	require(waitpid(child_pid, &status, 0) == child_pid, "waitpid");
	child_pid = -1;
	require(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child exit");
	if (sysv) {
		require(shmctl(shmid, IPC_RMID, NULL) == 0, "remove attached segment");
		/* Marked-for-removal memory remains usable until final detach. */
		verify(p, ROUNDS, 1);
		require(shmdt(p) == 0, "last shmdt");
		shmid = -1;
		require(semctl(semid, 0, IPC_RMID) == 0, "remove semaphores");
		semid = -1;
	} else {
		require(shm_unlink(shm_name) == 0, "unlink mapped object");
		verify(p, ROUNDS, 1);
		require(munmap(p, sizeof(*p)) == 0, "last munmap");
		mq_close(request);
		mq_close(reply);
		require(mq_unlink(request_name) == 0 && mq_unlink(reply_name) == 0, "unlink queues");
	}
	printf("IPC %s: %d page round trips between separate processes\n", sysv ? "System V" : "POSIX", ROUNDS);
}

struct shared_sync {
	pthread_mutex_t lock;
	pthread_cond_t condition;
	sem_t ready;
	unsigned sequence;
	pid_t writer;
};

static void pthread_require(int result, const char *what)
{
	if (result) errno = result;
	require(result == 0, what);
}

static struct shared_sync *sync_mapping(const char *name, int create)
{
	int fd = shm_open(name, O_RDWR | (create ? O_CREAT | O_EXCL : 0), 0600);
	require(fd >= 0, "process-shared shm_open");
	if (create)
		require(ftruncate(fd, sizeof(struct shared_sync)) == 0, "sync size");
	struct shared_sync *p = mmap(NULL, sizeof(*p), PROT_READ | PROT_WRITE,
				     MAP_SHARED, fd, 0);
	close(fd);
	require(p != MAP_FAILED, "sync mmap");
	return p;
}

static void sync_wait(struct shared_sync *p, unsigned sequence)
{
	struct timespec until = deadline();
	while (p->sequence != sequence)
		pthread_require(pthread_cond_timedwait(&p->condition, &p->lock, &until),
				"process-shared cond wait");
}

static int sync_child(const char *name)
{
	struct shared_sync *p = sync_mapping(name, 0);
	require(sem_post(&p->ready) == 0, "process-shared semaphore post");
	for (unsigned n = 0; n < ROUNDS; n++) {
		pthread_require(pthread_mutex_lock(&p->lock), "shared child lock");
		sync_wait(p, n * 2 + 1);
		require(p->writer != getpid(), "shared mutex parent identity");
		p->sequence++;
		p->writer = getpid();
		pthread_require(pthread_cond_signal(&p->condition), "shared child signal");
		pthread_require(pthread_mutex_unlock(&p->lock), "shared child unlock");
	}
	require(munmap(p, sizeof(*p)) == 0, "sync child munmap");
	return 0;
}

static void shared_synchronization(const char *program)
{
	struct shared_sync *p = sync_mapping(shm_name, 1);
	pthread_mutexattr_t ma;
	pthread_condattr_t ca;
	pthread_require(pthread_mutexattr_init(&ma), "shared mutex attr init");
	pthread_require(pthread_mutexattr_setpshared(&ma, PTHREAD_PROCESS_SHARED), "shared mutex attr");
	pthread_require(pthread_condattr_init(&ca), "shared cond attr init");
	pthread_require(pthread_condattr_setpshared(&ca, PTHREAD_PROCESS_SHARED), "shared cond attr");
	pthread_require(pthread_mutex_init(&p->lock, &ma), "shared mutex init");
	pthread_require(pthread_cond_init(&p->condition, &ca), "shared cond init");
	pthread_mutexattr_destroy(&ma);
	pthread_condattr_destroy(&ca);
	require(sem_init(&p->ready, 1, 0) == 0, "shared semaphore init");
	char *args[] = { (char *)program, "pshared-child", shm_name, NULL };
	pthread_require(posix_spawn(&child_pid, program, NULL, NULL, args, environ), "sync spawn");
	struct timespec until = deadline();
	require(sem_timedwait(&p->ready, &until) == 0, "shared semaphore wait");
	for (unsigned n = 0; n < ROUNDS; n++) {
		pthread_require(pthread_mutex_lock(&p->lock), "shared parent lock");
		p->sequence = n * 2 + 1;
		p->writer = getpid();
		pthread_require(pthread_cond_signal(&p->condition), "shared parent signal");
		sync_wait(p, n * 2 + 2);
		require(p->writer == child_pid, "shared mutex child identity");
		pthread_require(pthread_mutex_unlock(&p->lock), "shared parent unlock");
	}
	int status;
	require(waitpid(child_pid, &status, 0) == child_pid &&
		WIFEXITED(status) && WEXITSTATUS(status) == 0, "sync child exit");
	child_pid = -1;
	pthread_require(pthread_mutex_destroy(&p->lock), "shared mutex destroy");
	pthread_require(pthread_cond_destroy(&p->condition), "shared cond destroy");
	require(sem_destroy(&p->ready) == 0, "shared semaphore destroy");
	require(munmap(p, sizeof(*p)) == 0 && shm_unlink(shm_name) == 0, "sync cleanup");
	puts("IPC NPTL: process-shared mutex, condition variable and semaphore passed");
}

static void untouched_tail(const void *buffer, size_t used, size_t total)
{
	const unsigned char *bytes = buffer;
	for (size_t i = used; i < total; i++)
		require(bytes[i] == 0xa5, "IPC_INFO did not overwrite its buffer");
}

static void info_buffers(void)
{
	/* IPC_INFO returns shorter structures than IPC_STAT. Libc must not
	 * append time64 fields to these, or to a failed syscall's buffer. */
	union { struct shmid_ds ds; struct shminfo info; } shm;
	memset(&shm, 0xa5, sizeof(shm));
	require(shmctl(0, IPC_INFO, &shm.ds) >= 0, "shm IPC_INFO");
	untouched_tail(&shm, sizeof(shm.info), sizeof(shm));
	memset(&shm, 0xa5, sizeof(shm));
	require(shmctl(-1, IPC_STAT, &shm.ds) == -1, "invalid shm IPC_STAT");
	untouched_tail(&shm, 0, sizeof(shm));

	union { struct semid_ds ds; struct seminfo info; } sem;
	memset(&sem, 0xa5, sizeof(sem));
	require(semctl(0, 0, SEM_INFO, (union semun){ .buf = &sem.ds }) >= 0, "sem SEM_INFO");
	untouched_tail(&sem, sizeof(sem.info), sizeof(sem));

	union { struct msqid_ds ds; struct msginfo info; } msg;
	memset(&msg, 0xa5, sizeof(msg));
	require(msgctl(0, IPC_INFO, &msg.ds) >= 0, "msg IPC_INFO");
	untouched_tail(&msg, sizeof(msg.info), sizeof(msg));
}

static void queue_semantics(void)
{
	struct mq_attr attr = { .mq_maxmsg = 4, .mq_msgsize = 4 };
	mqd_t q = mq_open(request_name, O_CREAT | O_EXCL | O_RDWR | O_NONBLOCK, 0600, &attr);
	require(q != -1, "priority queue");
	char word[4] = "low", seen[4];
	require(mq_send(q, word, sizeof(word), 1) == 0, "low-priority send");
	memcpy(word, "high", 4);
	require(mq_send(q, word, sizeof(word), 9) == 0, "high-priority send");
	unsigned priority;
	require(mq_receive(q, seen, sizeof(seen), &priority) == 4 && priority == 9 && memcmp(seen, "high", 4) == 0, "message priority ordering");
	require(mq_receive(q, seen, sizeof(seen), &priority) == 4 && priority == 1, "remaining message");
	errno = 0;
	require(mq_receive(q, seen, sizeof(seen), NULL) == -1 && errno == EAGAIN, "nonblocking empty queue");
	struct mq_attr blocking = { .mq_flags = 0 };
	require(mq_setattr(q, &blocking, NULL) == 0, "blocking queue");
	struct timespec past = { 0, 0 };
	errno = 0;
	require(mq_timedreceive(q, seen, sizeof(seen), NULL, &past) == -1 && errno == ETIMEDOUT, "time64 receive timeout");
	mq_close(q);
	require(mq_unlink(request_name) == 0, "remove priority queue");

	/* System V messages are enabled alongside shared memory: exercise the
	 * stat buffer too, which has the same split-time ABI requirement. */
	msgid = msgget(IPC_PRIVATE, IPC_CREAT | 0600);
	require(msgid >= 0, "msgget");
	struct { long type; char text[4]; } message = { 1, "ipc" };
	require(msgsnd(msgid, &message, sizeof(message.text), 0) == 0, "msgsnd");
	struct msqid_ds info;
	require(msgctl(msgid, IPC_STAT, &info) == 0 && info.msg_qnum == 1 && info.msg_stime > 0, "msgctl time64 statistics");
	require(msgrcv(msgid, &message, sizeof(message.text), 1, 0) == 4 && strcmp(message.text, "ipc") == 0, "msgrcv");
	require(msgctl(msgid, IPC_RMID, NULL) == 0, "remove System V queue");
	msgid = -1;
}

int main(int argc, char **argv)
{
	if (argc == 3 && strcmp(argv[1], "pshared-child") == 0)
		return sync_child(argv[2]);
	if (argc > 1) return child(argc, argv);
	atexit(cleanup);
	snprintf(shm_name, sizeof(shm_name), "/wr-ipc-%ld", (long)getpid());
	snprintf(request_name, sizeof(request_name), "/wr-req-%ld", (long)getpid());
	snprintf(reply_name, sizeof(reply_name), "/wr-rep-%ld", (long)getpid());
	struct stat st;
	require(stat("/dev/shm", &st) == 0 && (st.st_mode & 07777) == 01777, "shared-memory directory mode");
	require(stat("/dev/mqueue", &st) == 0 && (st.st_mode & 07777) == 01777, "message-queue directory mode");
	exchange(argv[0], 0);
	exchange(argv[0], 1);
	shared_synchronization(argv[0]);
	queue_semantics();
	info_buffers();
	puts("IPC PASS: shared memory, semaphores, message queues and time64");
	return 0;
}
