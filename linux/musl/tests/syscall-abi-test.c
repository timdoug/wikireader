/* System call arguments as the kernel's wrappers read them from the frame
 * (arch/c33/include/asm/syscall_wrapper.h): 64-bit arguments at every word
 * position, high words included, six-argument calls, and a tracer's
 * register writes, which may change the call but not the kernel's own
 * words of the frame.  The file is on the root file system, ext4, which
 * holds sparse files past 4 GB. */
#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>
#include <asm/ptrace.h>

#define PATH "/sysabi.tmp"
#define COPY "/sysabi2.tmp"
#define G4 0x100000000LL

static int failures;
static char *self;

#define CHECK(cond) do {						\
	if (!(cond)) {							\
		printf("syscall-abi: %s:%d: %s (errno %d)\n",		\
		       __func__, __LINE__, #cond, errno);		\
		failures++;						\
	}								\
} while (0)

static off_t size_of(int fd)
{
	struct stat st;

	return fstat(fd, &st) ? -1 : st.st_size;
}

/* An offset at words 3-4: pread64 and pwrite64, the %r9:%r10 case. */
static void test_pread_pwrite(int fd)
{
	char buf[8] = { 0 };

	CHECK(pwrite(fd, "high", 4, G4 + 16) == 4);
	CHECK(size_of(fd) == G4 + 20);
	CHECK(pread(fd, buf, 4, G4 + 16) == 4 && !memcmp(buf, "high", 4));
	/* Nothing was written at the low word's offset alone. */
	CHECK(pread(fd, buf, 4, 16) == 4 && !memcmp(buf, "\0\0\0\0", 4));
	errno = 0;
	CHECK(pread(fd, buf, 4, -1) == -1 && errno == EINVAL);
}

/* A length at words 1-2: ftruncate64. */
static void test_ftruncate(int fd)
{
	CHECK(ftruncate(fd, 5 * G4) == 0);
	CHECK(size_of(fd) == 5 * G4);
	CHECK(ftruncate(fd, 0) == 0);
}

/* Two at words 2-3 and 4-5, the second once on the stack: fallocate. */
static void test_fallocate(int fd)
{
	CHECK(fallocate(fd, FALLOC_FL_KEEP_SIZE, 2 * G4, 4096) == 0);
	CHECK(size_of(fd) == 0);
	CHECK(fallocate(fd, 0, 3 * G4 + 8, 4096) == 0);
	CHECK(size_of(fd) == 3 * G4 + 8 + 4096);
	/* Negative only with its high word; the low word alone is 4096. */
	errno = 0;
	CHECK(fallocate(fd, 0, 0, 4096 - G4) == -1 && errno == EINVAL);
	CHECK(ftruncate(fd, 0) == 0);
}

/* Words 1-2 and 3-4, then flags in word 5. */
static void test_sync_file_range(int fd)
{
	CHECK(sync_file_range(fd, G4, G4, SYNC_FILE_RANGE_WRITE) == 0);
	errno = 0;
	CHECK(sync_file_range(fd, -G4, 4096, SYNC_FILE_RANGE_WRITE) == -1 &&
	      errno == EINVAL);
	errno = 0;
	CHECK(sync_file_range(fd, 0, 4096, 0x80) == -1 && errno == EINVAL);
}

/* fadvise64_64: words 1-2 and 3-4, the advice in word 5. */
static void test_fadvise(int fd)
{
	CHECK(posix_fadvise(fd, G4, G4, POSIX_FADV_WILLNEED) == 0);
	CHECK(posix_fadvise(fd, 0, -G4, POSIX_FADV_NORMAL) == EINVAL);
	CHECK(posix_fadvise(fd, 0, 0, 99) == EINVAL);
}

/* Six words, the last two read from %r10 and %r11. */
static void test_six_arguments(int fd)
{
	loff_t in = 4096, out = 3;
	char buf[8] = { 0 };
	int dst;
	char *map;

	CHECK(pwrite(fd, "copy", 4, 4096) == 4);
	dst = open(COPY, O_RDWR | O_CREAT | O_TRUNC, 0600);
	CHECK(dst >= 0);
	CHECK(copy_file_range(fd, &in, dst, &out, 4, 0) == 4);
	CHECK(in == 4100 && out == 7);
	CHECK(pread(dst, buf, 4, 3) == 4 && !memcmp(buf, "copy", 4));
	errno = 0;
	CHECK(copy_file_range(fd, &in, dst, &out, 4, 1) == -1 &&
	      errno == EINVAL);
	close(dst);
	unlink(COPY);

	/* mmap2: the page offset is the sixth word. */
	map = mmap(0, 4096, PROT_READ, MAP_PRIVATE, fd, 4096);
	CHECK(map != MAP_FAILED);
	if (map != MAP_FAILED) {
		CHECK(!memcmp(map, "copy", 4));
		munmap(map, 4096);
	}
}

static int regs_get(pid_t pid, struct user_pt_regs *regs)
{
	struct iovec io = { regs, sizeof *regs };

	return ptrace(PTRACE_GETREGSET, pid, NT_PRSTATUS, &io);
}

static int regs_set(pid_t pid, struct user_pt_regs *regs)
{
	struct iovec io = { regs, sizeof *regs };

	return ptrace(PTRACE_SETREGSET, pid, NT_PRSTATUS, &io);
}

/* At getpid's entry stop the tracer writes back every word it read with
 * vector, reserved and the PSR scrambled and the call changed to getppid.
 * The call is the tracer's; the scrambled words are not, and the child
 * returns to userspace, runs on and exits normally. */
static void traced(void)
{
	struct user_pt_regs regs, again;
	int status, entered = 0;
	pid_t pid;

	pid = vfork();
	if (pid == 0) {
		execl(self, self, "--child", (char *)0);
		_exit(127);
	}
	CHECK(pid > 0);
	if (pid <= 0)
		return;
	CHECK(waitpid(pid, &status, 0) == pid && WIFSTOPPED(status));
	CHECK(ptrace(PTRACE_SETOPTIONS, pid, 0, PTRACE_O_TRACESYSGOOD) == 0);
	for (;;) {
		CHECK(ptrace(PTRACE_SYSCALL, pid, 0, 0) == 0);
		if (waitpid(pid, &status, 0) != pid || !WIFSTOPPED(status))
			break;
		if (WSTOPSIG(status) != (SIGTRAP | 0x80) || regs_get(pid, &regs))
			continue;
		if (!entered && regs.orig_r4 == SYS_getpid) {
			unsigned long vector = regs.vector;
			unsigned long reserved = regs.reserved;
			unsigned long psr = regs.psr;

			entered = 1;
			regs.orig_r4 = SYS_getppid;
			regs.vector = 0x1234;
			regs.reserved = 0;
			regs.psr = psr | 0xfffffff0;
			CHECK(regs_set(pid, &regs) == 0);
			CHECK(regs_get(pid, &again) == 0);
			CHECK(again.orig_r4 == SYS_getppid);
			CHECK(again.vector == vector);
			CHECK(again.reserved == reserved && reserved != 0);
			CHECK(again.psr == ((psr & 0xf) | 0x10));
		}
	}
	CHECK(entered);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

/* Traced: getpid, which the tracer turns into getppid. */
static int child(void)
{
	pid_t parent = getppid();

	if (ptrace(PTRACE_TRACEME, 0, 0, 0))
		return 2;
	raise(SIGSTOP);
	return syscall(SYS_getpid) == parent ? 0 : 3;
}

int main(int argc, char **argv)
{
	int fd;

	if (argc > 1 && !strcmp(argv[1], "--child"))
		return child();
	self = argv[0];

	fd = open(PATH, O_RDWR | O_CREAT | O_TRUNC, 0600);
	CHECK(fd >= 0);
	if (fd >= 0) {
		test_pread_pwrite(fd);
		test_ftruncate(fd);
		test_fallocate(fd);
		test_sync_file_range(fd);
		test_fadvise(fd);
		test_six_arguments(fd);
		close(fd);
		unlink(PATH);
	}
	traced();

	if (failures) {
		printf("C33 syscall ABI test: %d FAILED\n", failures);
		return 1;
	}
	printf("C33 syscall ABI test passed\n");
	return 0;
}
