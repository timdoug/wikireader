// SPDX-License-Identifier: GPL-2.0
/* core_pattern pipe helper. Keep only the last crash, directly on the card:
 * no RAM-sized temporary file, shell, compression, or resident daemon. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CORE_MAX (4UL * 1024 * 1024)
#define CORE "/mnt/sd/crash.elf"
#define MAP "/mnt/sd/crash.map"
#define META "/mnt/sd/crash.txt"

static char buffer[16384];

static int output(const char *path)
{
	return open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
}

static int write_all(int fd, const char *data, size_t size)
{
	while (size) {
		ssize_t n = write(fd, data, size);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		data += n;
		size -= n;
	}
	return 0;
}

/* Continue consuming the pipe even after the cap, to let the kernel finish
 * cleanly. A truncated ELF still contains useful registers and mappings. */
static int copy(int in, int out, unsigned long cap, unsigned long *stored)
{
	int result = 0;
	ssize_t n;

	*stored = 0;
	for (;;) {
		n = read(in, buffer, sizeof(buffer));
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return n < 0 ? -1 : result;
		size_t keep = (unsigned long)n > cap ? cap : (unsigned long)n;
		if (keep && write_all(out, buffer, keep))
			return -1;
		*stored += keep;
		cap -= keep;
		if (keep < (size_t)n)
			result = 1;
	}
}

static unsigned long number(const char *text)
{
	char *end;
	unsigned long n;

	errno = 0;
	n = strtoul(text, &end, 10);
	/* An unlimited 64-bit rlimit overflows unsigned long on C33. */
	if (!*text || *end)
		return 0;
	return errno == ERANGE ? CORE_MAX : n;
}

int main(int argc, char **argv)
{
	char path[64], exe[256], build[80] = "unknown";
	unsigned long cap, bytes, maps;
	int in, out, meta, result;
	ssize_t n;

	if (argc != 5 || !number(argv[1]))
		return 1;
	cap = number(argv[4]);
	if (!cap)
		return 0;
	if (cap > CORE_MAX)
		cap = CORE_MAX;
	/* No stale sidecar should describe a newly overwritten core. */
	unlink(CORE);
	unlink(MAP);
	unlink(META);
	snprintf(path, sizeof(path), "/proc/%lu/maps", number(argv[1]));
	in = open(path, O_RDONLY);
	out = output(MAP);
	if (in < 0 || out < 0)
		return 1;
	result = copy(in, out, 65536, &maps);
	close(in);
	if (fsync(out) || close(out) || result)
		return 1;
	snprintf(path, sizeof(path), "/proc/%lu/exe", number(argv[1]));
	n = readlink(path, exe, sizeof(exe) - 1);
	if (n < 0)
		n = 0;
	exe[n] = 0;
	in = open("/etc/wr-build-id", O_RDONLY);
	if (in >= 0) {
		n = read(in, build, sizeof(build) - 1);
		if (n > 0) {
			build[n] = 0;
			build[strcspn(build, "\n")] = 0;
		}
		close(in);
	}
	out = output(CORE);
	if (out < 0)
		return 1;
	result = copy(STDIN_FILENO, out, cap, &bytes);
	if (fsync(out))
		result = -1;
	if (close(out))
		result = -1;
	meta = output(META);
	if (meta < 0)
		return 1;
	int length = snprintf(buffer, sizeof(buffer),
		"pid=%s\nsignal=%s\ntime=%s\nlimit=%s\nbuild=%s\nexe=%s\n"
		"bytes=%lu\nstatus=%s\n", argv[1], argv[2], argv[3], argv[4],
		build, exe, bytes,
		result < 0 ? "error" : result ? "truncated" : "complete");
	if (length < 0 || (size_t)length >= sizeof(buffer)) {
		close(meta);
		return 1;
	}
	result = write_all(meta, buffer, length) || fsync(meta) || result < 0;
	if (close(meta))
		result = 1;
	return result ? 1 : 0;
}
