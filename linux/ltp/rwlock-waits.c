/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "io-checks.h"
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
static pthread_rwlock_t lock = PTHREAD_RWLOCK_INITIALIZER;
static sem_t held, release;
static volatile sig_atomic_t signals;
static pthread_t main_thread;

static void token(sem_t *sem)
{
    while (sem_wait(sem) != 0)
        REQUIRE(errno == EINTR);
}

static void *holder(void *unused)
{
    (void)unused;
    REQUIRE(pthread_rwlock_wrlock(&lock) == 0);
    REQUIRE(sem_post(&held) == 0);
    token(&release);
    REQUIRE(pthread_rwlock_unlock(&lock) == 0);
    return NULL;
}

static void signal_handler(int sig)
{
    (void)sig;
    ++signals;
}

static void *interrupt(void *unused)
{
    (void)unused;
    for (int i = 0; i < 3; ++i) {
        pause_ns(10000000);
        REQUIRE(pthread_kill(main_thread, SIGUSR1) == 0);
    }
    return NULL;
}

static void *delayed_release(void *unused)
{
    (void)unused;
    pause_ns(30000000);
    REQUIRE(sem_post(&release) == 0);
    return NULL;
}

static struct timespec after_ns(long ns)
{
    struct timespec ts;
    REQUIRE(clock_gettime(CLOCK_REALTIME, &ts) == 0);
    ts.tv_nsec += ns;
    if (ts.tv_nsec >= 1000000000) {
        ++ts.tv_sec;
        ts.tv_nsec -= 1000000000;
    }
    return ts;
}

static void timeouts(void)
{
    struct sigaction action = {.sa_handler = signal_handler};
    sigemptyset(&action.sa_mask);
    REQUIRE(sigaction(SIGUSR1, &action, NULL) == 0);
    main_thread = pthread_self();
    REQUIRE(sem_init(&held, 0, 0) == 0);
    REQUIRE(sem_init(&release, 0, 0) == 0);
    pthread_t thread;
    REQUIRE(pthread_create(&thread, NULL, holder, NULL) == 0);
    token(&held);
    struct timespec bad[] = {{0, -1}, {0, 1000000000}, {-1, 0}, {0, 0}};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        int expected = i < 2 ? EINVAL : ETIMEDOUT;
        CHECK(pthread_rwlock_timedrdlock(&lock, &bad[i]) == expected);
        CHECK(pthread_rwlock_timedwrlock(&lock, &bad[i]) == expected);
    }
    for (int writer = 0; writer < 2; ++writer) {
        pthread_t sender;
        REQUIRE(pthread_create(&sender, NULL, interrupt, NULL) == 0);
        struct timespec ts = after_ns(100000000);
        int64_t before = now_ns();
        errno = 123;
        int ret = writer ? pthread_rwlock_timedwrlock(&lock, &ts)
                         : pthread_rwlock_timedrdlock(&lock, &ts);
        CHECK(ret == ETIMEDOUT && errno == 123);
        int64_t elapsed = now_ns() - before;
        CHECK(elapsed >= 80000000 && elapsed < 2000000000LL);
        REQUIRE(pthread_join(sender, NULL) == 0);
    }
    CHECK(signals == 6);
    REQUIRE(sem_post(&release) == 0);
    REQUIRE(pthread_join(thread, NULL) == 0);
    /* Immediate acquisition ignores an invalid timeout, as POSIX permits. */
    CHECK(pthread_rwlock_timedrdlock(&lock, &bad[0]) == 0);
    REQUIRE(pthread_rwlock_unlock(&lock) == 0);
    CHECK(pthread_rwlock_timedwrlock(&lock, &bad[1]) == 0);
    REQUIRE(pthread_rwlock_unlock(&lock) == 0);
    REQUIRE(sem_destroy(&held) == 0 && sem_destroy(&release) == 0);

    /* An actually blocked post-2038 deadline must survive conversion. */
    REQUIRE(sem_init(&held, 0, 0) == 0);
    REQUIRE(sem_init(&release, 0, 0) == 0);
    REQUIRE(pthread_create(&thread, NULL, holder, NULL) == 0);
    token(&held);
    struct timespec wide = {2147483999LL, 123456789};
    pthread_t releaser;
    REQUIRE(pthread_create(&releaser, NULL, delayed_release, NULL) == 0);
    CHECK(pthread_rwlock_timedrdlock(&lock, &wide) == 0);
    REQUIRE(pthread_rwlock_unlock(&lock) == 0);
    REQUIRE(pthread_join(thread, NULL) == 0);
    REQUIRE(pthread_join(releaser, NULL) == 0);
    REQUIRE(sem_destroy(&held) == 0 && sem_destroy(&release) == 0);
    printf("rwlock deadlines, EINTR retry, errno and queue removal\n");
}

static int pair_a, pair_b;
static void *contend(void *arg)
{
    int writer = (long)arg;
    for (int i = 0; i < 128; ++i) {
        REQUIRE((writer ? pthread_rwlock_wrlock(&lock) : pthread_rwlock_rdlock(&lock)) == 0);
        if (writer) {
            int value = pair_a + 1;
            pair_a = value;
            sched_yield();
            pair_b = value;
        } else {
            int value = pair_a;
            sched_yield();
            REQUIRE(value == pair_b);
        }
        REQUIRE(pthread_rwlock_unlock(&lock) == 0);
    }
    return NULL;
}

static void contention(void)
{
    pthread_t threads[4];
    for (int i = 0; i < 4; ++i)
        REQUIRE(pthread_create(&threads[i], NULL, contend, (void *)(long)(i < 2)) == 0);
    for (int i = 0; i < 4; ++i)
        REQUIRE(pthread_join(threads[i], NULL) == 0);
    CHECK(pair_a == 256 && pair_b == 256);
    printf("512 contended read/write operations preserve protected data\n");
}

static int expired_writer_result, waiting_reader_done;
static void *expiring_writer(void *unused)
{
    (void)unused;
    struct timespec ts = after_ns(100000000);
    REQUIRE(sem_post(&held) == 0);
    expired_writer_result = pthread_rwlock_timedwrlock(&lock, &ts);
    if (!expired_writer_result)
        REQUIRE(pthread_rwlock_unlock(&lock) == 0);
    return NULL;
}

static void *waiting_reader(void *unused)
{
    (void)unused;
    REQUIRE(pthread_rwlock_rdlock(&lock) == 0);
    __atomic_store_n(&waiting_reader_done, 1, __ATOMIC_RELEASE);
    REQUIRE(pthread_rwlock_unlock(&lock) == 0);
    return NULL;
}

static void last_writer_timeout(void)
{
    pthread_rwlockattr_t attr;
    REQUIRE(pthread_rwlock_destroy(&lock) == 0);
    REQUIRE(pthread_rwlockattr_init(&attr) == 0);
    REQUIRE(pthread_rwlockattr_setkind_np(&attr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP) == 0);
    REQUIRE(pthread_rwlock_init(&lock, &attr) == 0);
    REQUIRE(pthread_rwlockattr_destroy(&attr) == 0);
    REQUIRE(sem_init(&held, 0, 0) == 0);
    REQUIRE(pthread_rwlock_rdlock(&lock) == 0);
    pthread_t writer, reader;
    REQUIRE(pthread_create(&writer, NULL, expiring_writer, NULL) == 0);
    token(&held);
    pause_ns(10000000);
    REQUIRE(pthread_create(&reader, NULL, waiting_reader, NULL) == 0);
    REQUIRE(pthread_join(writer, NULL) == 0);
    CHECK(expired_writer_result == ETIMEDOUT);
    int64_t limit = now_ns() + 1000000000LL;
    while (!__atomic_load_n(&waiting_reader_done, __ATOMIC_ACQUIRE) && now_ns() < limit)
        pause_ns(1000000);
    /* Parent still holds a read lock: timeout cleanup must release the
     * reader waiting behind the last writer without requiring unlock. */
    CHECK(__atomic_load_n(&waiting_reader_done, __ATOMIC_ACQUIRE));
    REQUIRE(pthread_rwlock_unlock(&lock) == 0);
    REQUIRE(pthread_join(reader, NULL) == 0);
    REQUIRE(sem_destroy(&held) == 0);
    printf("last queued writer timeout releases blocked readers\n");
}

static void owners(void)
{
    /* More than four distinct locks exercises ownership-record overflow.
     * Repeating acquisition/release also checks embedded-slot reuse. */
    pthread_rwlock_t locks[9];
    for (int i = 0; i < 9; ++i)
        REQUIRE(pthread_rwlock_init(&locks[i], NULL) == 0);
    for (int repeat = 0; repeat < 20; ++repeat) {
        for (int i = 0; i < 9; ++i) {
            REQUIRE(pthread_rwlock_rdlock(&locks[i]) == 0);
            REQUIRE(pthread_rwlock_tryrdlock(&locks[i]) == 0);
        }
        for (int i = 8; i >= 0; --i) {
            REQUIRE(pthread_rwlock_unlock(&locks[i]) == 0);
            REQUIRE(pthread_rwlock_unlock(&locks[i]) == 0);
            REQUIRE(pthread_rwlock_trywrlock(&locks[i]) == 0);
            REQUIRE(pthread_rwlock_unlock(&locks[i]) == 0);
        }
    }
    for (int i = 0; i < 9; ++i)
        REQUIRE(pthread_rwlock_destroy(&locks[i]) == 0);
}

struct shared {
    pthread_rwlock_t lock;
    sem_t ready[2], acquired[2], go[2];
    unsigned long address[2];
    int order[2], count;
};

static int child(char **argv)
{
    int id = atoi(argv[3]);
    struct shared *s = shmat(atoi(argv[2]), NULL, 0);
    REQUIRE(s != (void *)-1);
    struct sched_param p = {.sched_priority = id ? 3 : 1};
    REQUIRE(sched_setscheduler(0, SCHED_FIFO, &p) == 0);
    s->address[id] = (unsigned long)s;
    REQUIRE(sem_post(&s->ready[id]) == 0);
    struct timespec ts = after_ns(0);
    ts.tv_sec += 5;
    REQUIRE((id ? pthread_rwlock_timedrdlock(&s->lock, &ts)
                : pthread_rwlock_timedwrlock(&s->lock, &ts)) == 0);
    int n = __atomic_fetch_add(&s->count, 1, __ATOMIC_SEQ_CST);
    s->order[n] = id;
    if (id) {
        REQUIRE(pthread_rwlock_tryrdlock(&s->lock) == 0);
        REQUIRE(pthread_rwlock_unlock(&s->lock) == 0);
    }
    REQUIRE(sem_post(&s->acquired[id]) == 0);
    token(&s->go[id]);
    REQUIRE(pthread_rwlock_unlock(&s->lock) == 0);
    REQUIRE(shmdt(s) == 0);
    return 0;
}

static void processes(const char *program)
{
    int id = shmget(IPC_PRIVATE, sizeof(struct shared), IPC_CREAT | 0600);
    REQUIRE(id >= 0);
    struct shared *s = shmat(id, NULL, 0);
    REQUIRE(s != (void *)-1);
    pthread_rwlockattr_t attr;
    REQUIRE(pthread_rwlockattr_init(&attr) == 0);
    REQUIRE(pthread_rwlockattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) == 0);
    REQUIRE(pthread_rwlock_init(&s->lock, &attr) == 0);
    REQUIRE(pthread_rwlockattr_destroy(&attr) == 0);
    REQUIRE(pthread_rwlock_wrlock(&s->lock) == 0);
    pid_t children[2];
    char object[24];
    snprintf(object, sizeof(object), "%d", id);
    for (int i = 0; i < 2; ++i) {
        REQUIRE(sem_init(&s->ready[i], 1, 0) == 0);
        REQUIRE(sem_init(&s->acquired[i], 1, 0) == 0);
        REQUIRE(sem_init(&s->go[i], 1, 0) == 0);
        char index[2] = {'0' + i, 0};
        char *args[] = {(char *)program, "child", object, index, NULL};
        REQUIRE(posix_spawn(&children[i], program, NULL, NULL, args, environ) == 0);
        token(&s->ready[i]);
        pause_ns(30000000);
    }
    REQUIRE(pthread_rwlock_unlock(&s->lock) == 0);
    token(&s->acquired[1]);
    CHECK(s->count == 1 && s->order[0] == 1);
    CHECK(pthread_rwlock_trywrlock(&s->lock) == EBUSY);
    CHECK(pthread_rwlock_tryrdlock(&s->lock) == EBUSY);
    REQUIRE(sem_post(&s->go[1]) == 0);
    token(&s->acquired[0]);
    CHECK(s->count == 2 && s->order[1] == 0);
    REQUIRE(sem_post(&s->go[0]) == 0);
    for (int i = 0; i < 2; ++i) {
        int status;
        REQUIRE(waitpid(children[i], &status, 0) == children[i]);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
#ifdef __c33__
        CHECK(s->address[i] == (unsigned long)s);
#endif
        REQUIRE(sem_destroy(&s->ready[i]) == 0);
        REQUIRE(sem_destroy(&s->acquired[i]) == 0);
        REQUIRE(sem_destroy(&s->go[i]) == 0);
    }
    REQUIRE(pthread_rwlock_destroy(&s->lock) == 0);
    REQUIRE(shmdt(s) == 0);
    REQUIRE(shmctl(id, IPC_RMID, NULL) == 0);
    printf("separately exec'd processes: reader before lower-priority writer\n");
}

int main(int argc, char **argv)
{
    if (argc == 4)
        return child(argv);
    timeouts();
    owners();
    contention();
    last_writer_timeout();
    processes(argv[0]);
    CHECK(pthread_rwlock_destroy(&lock) == 0);
    return finish("rwlock deadlines, signals, ownership reuse and process-shared priority handoff");
}
