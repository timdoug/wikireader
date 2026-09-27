// SPDX-License-Identifier: GPL-2.0
/*
 * PID 1 of the boot initramfs.  The system itself is an ext4 image,
 * linux.img, in the SD card's FAT partition beside linux.app: wait for the
 * card, mount it, set the clock, attach the image to a loop device, mount
 * that, credit the random seed the last boot saved, move the card to
 * /mnt/sd inside the image, and become its /sbin/init.
 *
 * With no C library, this is freestanding and its system calls go through
 * c33_syscall.  If anything fails there is no shell to fall back to, so it
 * says why on the console and, when it can, in linuxboot.txt on the card,
 * and reboots to the launcher.
 */
typedef unsigned long size_t;

long c33_syscall(long a1, long a2, long a3, long a4, long a5, long nr);

#define NR_ioctl			29
#define NR_renameat2			276
#define NR_statx			291
#define NR_clock_settime64		404
#define NR_mount			40
#define NR_chdir			49
#define NR_chroot			51
#define NR_openat			56
#define NR_close			57
#define NR_read				63
#define NR_write			64
#define NR_sync				81
#define NR_reboot			142
#define NR_execve			221
#define NR_clock_nanosleep_time64	407

#define AT_FDCWD	-100
#define O_RDONLY	00
#define O_WRONLY	01
#define O_RDWR		02
#define O_CREAT		0100
#define O_TRUNC		01000
#define MS_SYNCHRONOUS	16
#define MS_NOATIME	1024
#define MS_MOVE		8192
#define LOOP_SET_FD	0x4c00
#define LOOP_SET_DIRECT_IO	0x4c08
#define RNDADDENTROPY	0x40085203
#define CLOCK_REALTIME	0
#define CLOCK_MONOTONIC	1
#define STATX_MTIME	0x40
#define STATX_MTIME_OFFSET	112	/* struct statx's stx_mtime.tv_sec */

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

static void put(int fd, const char *s)
{
	sys(NR_write, fd, s, length(s), 0, 0);
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
	struct { long long sec, nsec; } delay = { 0, 50000000 };

	sys(NR_clock_nanosleep_time64, CLOCK_MONOTONIC, 0, &delay, 0, 0);
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
	static long long statx[32];
	long long newest = 0, when;
	struct { long long sec, nsec; } now = { 0, 0 };
	unsigned int i;

	for (i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
		if (sys(NR_statx, AT_FDCWD, files[i], 0, STATX_MTIME, statx))
			continue;
		when = statx[STATX_MTIME_OFFSET / 8];
		if (when > newest)
			newest = when;
	}
	if (newest < 1262304000) {
		put(1, "C33 time: no clock source, running from the epoch\n");
		return;
	}
	now.sec = newest;
	sys(NR_clock_settime64, CLOCK_REALTIME, &now, 0, 0, 0);
	put(1, "C33 time: clock set from the card, @");
	put_number(1, (long)newest);
	put(1, "\n");
}

/*
 * Credit the seed BusyBox seedrng saved last time, so the kernel's random
 * pool is ready before the first program runs: without it, the first read
 * of /dev/urandom waits while the kernel gathers jitter entropy, several
 * seconds on this CPU.  The seed is then renamed so that it is never
 * credited twice; rcS runs seedrng, which mixes it in again uncredited and
 * saves a fresh one for the next boot.
 */
static void credit_seed(void)
{
	static struct {
		int bits;
		int size;
		unsigned char data[SEED_MAX];
	} seed;
	long fd, random, size;

	fd = sys(NR_openat, AT_FDCWD, SEED_DIR "/seed.credit", O_RDONLY, 0, 0);
	if (fd < 0) {
		put(1, "C33 random: no saved seed\n");
		return;
	}
	size = sys(NR_read, fd, seed.data, SEED_MAX, 0, 0);
	sys(NR_close, fd, 0, 0, 0, 0);
	random = sys(NR_openat, AT_FDCWD, "/dev/urandom", O_WRONLY, 0, 0);
	if (size > 0 && random >= 0) {
		seed.bits = size * 8;
		seed.size = size;
		if (!sys(NR_ioctl, random, RNDADDENTROPY, &seed, 0, 0)) {
			put(1, "C33 random: credited a ");
			put_number(1, size * 8);
			put(1, "-bit seed from the last boot\n");
		}
	}
	if (random >= 0)
		sys(NR_close, random, 0, 0, 0, 0);
	sys(NR_renameat2, AT_FDCWD, SEED_DIR "/seed.credit", AT_FDCWD,
	    SEED_DIR "/seed.no-credit", 0);
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
		fd = sys(NR_openat, AT_FDCWD, path, O_WRONLY | O_CREAT | O_TRUNC,
			 0644, 0);
		if (fd >= 0) {
			report(fd, why, error);
			sys(NR_close, fd, 0, 0, 0, 0);
		}
	}
	sys(NR_sync, 0, 0, 0, 0, 0);
	sys(NR_reboot, 0xfee1dead, 672274793, 0x01234567, 0, 0);
	for (;;)
		;
}

void root_main(void)
{
	static char *const argv[] = { "/sbin/init", 0 };
	static char *const envp[] = { "HOME=/", "TERM=linux", 0 };
	long error = 0, image, loop;
	int i;

	/* The card is probed while PID 1 starts; give it time to appear.
	 * Synchronous, as the system has always mounted it: nothing unmounts
	 * it before the power goes.  usefree takes the free count the card
	 * records: attaching the loop device asks for it, and otherwise the
	 * whole FAT is read and scanned to count, half a second at boot. */
	for (i = 0; i < CARD_TRIES; i++) {
		error = sys(NR_mount, CARD, "/mnt", "vfat", MS_SYNCHRONOUS,
			    "usefree");
		if (!error)
			break;
		sleep_50ms();
	}
	if (error)
		fail("no FAT partition on the SD card", error);
	card_dir = "/mnt";
	/* Before ext4 is mounted, so that it records the time too. */
	set_clock();

	image = sys(NR_openat, AT_FDCWD, "/mnt/" IMAGE, O_RDWR, 0, 0);
	if (image < 0)
		fail("no " IMAGE " on the SD card", image);
	loop = sys(NR_openat, AT_FDCWD, "/dev/loop0", O_RDWR, 0, 0);
	if (loop < 0)
		fail("no loop device", loop);
	error = sys(NR_ioctl, loop, LOOP_SET_FD, image, 0, 0);
	if (error)
		fail("could not attach " IMAGE, error);
	/* Straight to the card: through the FAT's page cache, every small ext4
	 * read would also start the file's own readahead, and the image would
	 * be cached twice.  Buffered I/O still works if this is refused. */
	sys(NR_ioctl, loop, LOOP_SET_DIRECT_IO, 1, 0, 0);
	sys(NR_close, image, 0, 0, 0, 0);
	sys(NR_close, loop, 0, 0, 0, 0);

	error = sys(NR_mount, "/dev/loop0", "/newroot", "ext4", MS_NOATIME, "");
	if (error)
		fail(IMAGE " is not a usable ext4 image", error);
	credit_seed();
	error = sys(NR_mount, "/mnt", "/newroot/mnt/sd", 0, MS_MOVE, 0);
	if (error)
		fail(IMAGE " has no /mnt/sd", error);
	card_dir = "/newroot/mnt/sd";

	sys(NR_chdir, "/newroot", 0, 0, 0, 0);
	error = sys(NR_mount, ".", "/", 0, MS_MOVE, 0);
	if (!error)
		error = sys(NR_chroot, ".", 0, 0, 0, 0);
	if (error)
		fail("could not switch to " IMAGE, error);
	card_dir = "/mnt/sd";
	sys(NR_chdir, "/", 0, 0, 0, 0);

	put(1, "C33 root: running " IMAGE " from the SD card\n");
	error = sys(NR_execve, argv[0], argv, envp, 0, 0);
	fail(IMAGE " has no /sbin/init", error);
}
