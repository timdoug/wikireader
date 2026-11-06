// SPDX-License-Identifier: GPL-2.0
/*
 * PID 1 of the boot initramfs.  The system itself is an ext4 image,
 * linux.img, in the SD card's FAT partition beside linux.app: wait for the
 * card, mount it, set the clock, attach the image to a loop device, mount
 * that, credit the random seed the last boot saved, move the card to
 * /mnt/sd inside the image, mount the kernel's file systems there, and
 * become its /sbin/init.
 *
 * With no C library, this is freestanding and its system calls go through
 * c33_syscall.  If anything fails there is no shell to fall back to, so it
 * says why on the console and, when it can, in linuxboot.txt on the card,
 * and reboots to the launcher.
 */
#include <stddef.h>

/* The kernel's own definitions: the sysroot's UAPI headers need no libc. */
#include <asm/unistd.h>
#include <linux/fcntl.h>
#include <linux/loop.h>
#include <linux/mount.h>
#include <linux/random.h>
#include <linux/reboot.h>
#include <linux/resource.h>
#include <linux/stat.h>
#include <linux/time.h>
#include <linux/time_types.h>

long c33_syscall(long a1, long a2, long a3, long a4, long a5, long nr);


#define sys(nr, a, b, c, d, e) \
	c33_syscall((long)(a), (long)(b), (long)(c), (long)(d), (long)(e), nr)

/* The card, the loop device, the console and urandom are nodes in the
 * initramfs: devtmpfs is left for the real system to mount. */
#define CARD		"/dev/mmcblk0p1"
#define IMAGE		"linux.img"
#define CARD_TRIES	200	/* 50 ms each */
#define SEED_DIR	"/newroot/var/lib/seedrng"
#define SEED_MAX	256	/* BusyBox seedrng's */

static const char *card_dir;	/* where the FAT is mounted, once it is */

static size_t length(const char *s)
{
	size_t n = 0;

	while (s[n])
		n++;
	return n;
}

/*
 * Line-buffered, as stdio is: a line goes out in one write, which the
 * kernel's console thread, printing between a program's writes, cannot
 * split.
 */
static void put(int fd, const char *s)
{
	static char line[128];
	static size_t used;

	for (; *s; s++) {
		line[used++] = *s;
		if (*s == '\n' || used == sizeof(line)) {
			sys(__NR_write, fd, line, used, 0, 0);
			used = 0;
		}
	}
}

static void put_number(int fd, long value)
{
	char digits[12];
	int i = sizeof(digits);
	unsigned long n = value < 0 ? -value : value;

	digits[--i] = '\0';
	do {
		digits[--i] = '0' + n % 10;
		n /= 10;
	} while (n);
	if (value < 0)
		digits[--i] = '-';
	put(fd, digits + i);
}

static void sleep_50ms(void)
{
	struct __kernel_timespec delay = { 0, 50000000 };

	sys(__NR_clock_nanosleep_time64, CLOCK_MONOTONIC, 0, &delay, 0, 0);
}

/*
 * There is no RTC with a backup cell, so the clock starts at the epoch.
 * The newest of these files is the best estimate there is: linux.img
 * changes whenever the running system writes, so the clock carries on from
 * the last session rather than from the last install.  2010 is the floor;
 * below it the card is showing FAT's floor, not a date.
 */
static void set_clock(void)
{
	static const char *const files[] = {
		"/mnt/" IMAGE, "/mnt/linux.app", "/mnt/kernel.elf",
	};
	static struct statx st;
	long long newest = 0, when;
	struct __kernel_timespec now = { 0, 0 };
	unsigned int i;

	for (i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
		if (sys(__NR_statx, AT_FDCWD, files[i], 0, STATX_MTIME, &st))
			continue;
		when = st.stx_mtime.tv_sec;
		if (when > newest)
			newest = when;
	}
	if (newest < 1262304000) {
		put(1, "C33 time: no clock source, running from the epoch\n");
		return;
	}
	now.tv_sec = newest;
	sys(__NR_clock_settime64, CLOCK_REALTIME, &now, 0, 0, 0);
	put(1, "C33 time: clock set from the card, @");
	put_number(1, (long)newest);
	put(1, "\n");
}

/*
 * Credit the seed BusyBox seedrng saved last time, so the kernel's random
 * pool is ready before the first program runs: without it, the first read
 * of /dev/urandom waits while the kernel gathers jitter entropy, several
 * seconds on this CPU.  The seed is then renamed so that it is never
 * credited twice; late runs seedrng, which mixes it in again uncredited and
 * saves a fresh one for the next boot.  An image's first boot has none.
 */
static void credit_seed(void)
{
	/* struct rand_pool_info with its buffer. */
	static struct {
		int bits;
		int size;
		unsigned char data[SEED_MAX];
	} seed;
	_Static_assert(offsetof(struct rand_pool_info, buf) ==
		       offsetof(typeof(seed), data), "rand_pool_info layout");
	long fd, random, size;

	fd = sys(__NR_openat, AT_FDCWD, SEED_DIR "/seed.credit", O_RDONLY, 0, 0);
	if (fd < 0) {
		put(1, "C33 random: no saved seed\n");
		return;
	}
	size = sys(__NR_read, fd, seed.data, SEED_MAX, 0, 0);
	sys(__NR_close, fd, 0, 0, 0, 0);
	random = sys(__NR_openat, AT_FDCWD, "/dev/urandom", O_WRONLY, 0, 0);
	if (size > 0 && random >= 0) {
		seed.bits = size * 8;
		seed.size = size;
		if (!sys(__NR_ioctl, random, RNDADDENTROPY, &seed, 0, 0)) {
			put(1, "C33 random: credited a ");
			put_number(1, size * 8);
			put(1, "-bit seed from the last boot\n");
		}
	}
	if (random >= 0)
		sys(__NR_close, random, 0, 0, 0, 0);
	sys(__NR_renameat2, AT_FDCWD, SEED_DIR "/seed.credit", AT_FDCWD,
	    SEED_DIR "/seed.no-credit", 0);
}

/*
 * The kernel registers a VT's device only once the VT is allocated, so
 * devtmpfs holds just the VTs in use.  Programs that name a VT, such as
 * openvt -c 3, expect its node beforehand, as a static /dev has them.
 */
static void make_vt_nodes(void)
{
	static char path[] = "/newroot/dev/ttyNN";
	unsigned int n;

	for (n = 1; n <= 12; n++) {
		char *p = path + sizeof("/newroot/dev/tty") - 1;

		if (n >= 10)
			*p++ = '1';
		*p++ = '0' + n % 10;
		*p = 0;
		/* Character device 4:n, the kernel's encoding of the dev_t. */
		sys(__NR_mknodat, AT_FDCWD, path, S_IFCHR | 0600, (4 << 8) | n, 0);
	}
}

/*
 * /proc, /sys, /dev and /dev/pts, here rather than from rcS: each mount
 * there is an exec of BusyBox, about a tenth of a second.  A failure is
 * said and survived; rcS checks for the PTYs the console needs.
 */
static void mount_kernel_fs(void)
{
	/* /tmp and /run in RAM, as Buildroot's own fstab has them: X's log,
	 * lock and socket written to the card after late has checkpointed the
	 * journal were replayed at the next boot, 1.6 s of mounting. */
	static const char *const fs[][3] = {
		{ "proc", "/newroot/proc", 0 },
		{ "sysfs", "/newroot/sys", 0 },
		{ "devtmpfs", "/newroot/dev", 0 },
		{ "devpts", "/newroot/dev/pts", 0 },
		{ "tmpfs", "/newroot/tmp", "mode=1777" },
		{ "tmpfs", "/newroot/run", "mode=0755" },
		{ "tmpfs", "/newroot/dev/shm", "mode=1777" },
		{ "mqueue", "/newroot/dev/mqueue", 0 },
	};
	unsigned int i;
	long error;

	for (i = 0; i < sizeof(fs) / sizeof(fs[0]); i++) {
		/* These directories live on the newly mounted devtmpfs. */
		if (i == 3 || i > 5)
			sys(__NR_mkdirat, AT_FDCWD, fs[i][1], 0755, 0, 0);
		error = sys(__NR_mount, fs[i][0], fs[i][1], fs[i][0],
			    i > 3 ? MS_NOSUID | MS_NODEV : 0, (long)fs[i][2]);
		if (error) {
			put(1, "C33 root: could not mount ");
			put(1, fs[i][1] + sizeof("/newroot") - 1);
			put(1, " (error ");
			put_number(1, error);
			put(1, ")\n");
		}
	}
	make_vt_nodes();
}

static void report(int fd, const char *why, long error)
{
	put(fd, "C33 root: ");
	put(fd, why);
	put(fd, " (error ");
	put_number(fd, error);
	put(fd, "); returning to the launcher\n");
}

static void __attribute__((noreturn)) fail(const char *why, long error)
{
	report(1, why, error);
	if (card_dir) {
		static char path[64];
		size_t n = length(card_dir);
		long fd;

		for (size_t i = 0; i < n; i++)
			path[i] = card_dir[i];
		for (size_t i = 0; i <= sizeof("/linuxboot.txt") - 1; i++)
			path[n + i] = "/linuxboot.txt"[i];
		fd = sys(__NR_openat, AT_FDCWD, path, O_WRONLY | O_CREAT | O_TRUNC,
			 0644, 0);
		if (fd >= 0) {
			report(fd, why, error);
			sys(__NR_close, fd, 0, 0, 0, 0);
		}
	}
	sys(__NR_sync, 0, 0, 0, 0, 0);
	sys(__NR_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
	    LINUX_REBOOT_CMD_RESTART, 0, 0);
	for (;;)
		;
}

void root_main(void)
{
	static const struct rlimit64 core_limit = {
		.rlim_cur = 4 * 1024 * 1024, .rlim_max = RLIM64_INFINITY,
	};
	static char *const argv[] = { "/sbin/init", 0 };
	static char *const envp[] = { "HOME=/root", "TERM=linux", "LANG=C.UTF-8", 0 };
	long error = 0, image, loop;
	int i;

	/* The card is probed while PID 1 starts; give it time to appear.
	 * Opening the partition fails quietly until it exists, where a mount
	 * of it would log "Can't open blockdev" each time, so the mount waits
	 * for the open.  Synchronous, as the system has always mounted it:
	 * nothing unmounts it before the power goes.  usefree takes the free
	 * count the card records: attaching the loop device asks for it, and
	 * otherwise the whole FAT is read and scanned to count, half a second
	 * at boot. */
	for (i = 0; i < CARD_TRIES; i++) {
		long fd = sys(__NR_openat, AT_FDCWD, CARD, O_RDONLY, 0, 0);

		if (fd >= 0) {
			sys(__NR_close, fd, 0, 0, 0, 0);
			error = sys(__NR_mount, CARD, "/mnt", "vfat",
				    MS_SYNCHRONOUS, "usefree");
			if (!error)
				break;
		} else {
			error = fd;
		}
		sleep_50ms();
	}
	if (error)
		fail("no FAT partition on the SD card", error);
	card_dir = "/mnt";
	/* Before ext4 is mounted, so that it records the time too. */
	set_clock();

	image = sys(__NR_openat, AT_FDCWD, "/mnt/" IMAGE, O_RDWR, 0, 0);
	if (image < 0)
		fail("no " IMAGE " on the SD card", image);
	loop = sys(__NR_openat, AT_FDCWD, "/dev/loop0", O_RDWR, 0, 0);
	if (loop < 0)
		fail("no loop device", loop);
	error = sys(__NR_ioctl, loop, LOOP_SET_FD, image, 0, 0);
	if (error)
		fail("could not attach " IMAGE, error);
	/* Straight to the card: through the FAT's page cache, every small ext4
	 * read would also start the file's own readahead, and the image would
	 * be cached twice.  Buffered I/O still works if this is refused. */
	sys(__NR_ioctl, loop, LOOP_SET_DIRECT_IO, 1, 0, 0);
	sys(__NR_close, image, 0, 0, 0, 0);
	sys(__NR_close, loop, 0, 0, 0, 0);

	error = sys(__NR_mount, "/dev/loop0", "/newroot", "ext4", MS_NOATIME, "");
	if (error)
		fail(IMAGE " is not a usable ext4 image", error);
	credit_seed();
	error = sys(__NR_mount, "/mnt", "/newroot/mnt/sd", 0, MS_MOVE, 0);
	if (error)
		fail(IMAGE " has no /mnt/sd", error);
	card_dir = "/newroot/mnt/sd";
	mount_kernel_fs();

	sys(__NR_chdir, "/newroot", 0, 0, 0, 0);
	error = sys(__NR_mount, ".", "/", 0, MS_MOVE, 0);
	if (!error)
		error = sys(__NR_chroot, ".", 0, 0, 0, 0);
	if (error)
		fail("could not switch to " IMAGE, error);
	card_dir = "/mnt/sd";
	sys(__NR_chdir, "/", 0, 0, 0, 0);

	put(1, "C33 root: running " IMAGE " from the SD card\n");
	/* Inherited by init and every service; shells can lower it to zero.
	 * The pipe collector also enforces this limit, since Linux itself
	 * does not enforce RLIMIT_CORE for piped dumps. */
	error = sys(__NR_prlimit64, 0, RLIMIT_CORE, &core_limit, 0, 0);
	if (error)
		fail("could not set the core size limit", error);
	error = sys(__NR_execve, argv[0], argv, envp, 0, 0);
	fail(IMAGE " has no /sbin/init", error);
}
