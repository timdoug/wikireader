#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

static int check_file(const char *name, unsigned int multiplier,
		      unsigned int bias, unsigned int length)
{
	unsigned char buffer[4096];
	unsigned int offset = 0, i;
	int fd = open(name, O_RDONLY);
	if (fd < 0)
		return -1;
	while (offset < length) {
		ssize_t count = read(fd, buffer, sizeof(buffer));
		if (count <= 0) {
			close(fd);
			return -1;
		}
		for (i = 0; i < (unsigned int)count; ++i)
			if (buffer[i] != (unsigned char)((offset + i) * multiplier + bias)) {
				close(fd);
				return -1;
			}
		offset += count;
	}
	return close(fd);
}

int main(void)
{
	struct timespec start, now;
	int ok = !check_file("/mnt/sd/sda.dat", 17, 7, 4096);
	if (clock_gettime(CLOCK_MONOTONIC, &start))
		return 2;
	/* Leave the DMA prefetch ring full before switching to another file. */
	do {
		if (clock_gettime(CLOCK_MONOTONIC, &now))
			return 2;
	} while (now.tv_sec - start.tv_sec < 3);
	if (check_file("/mnt/sd/sdb.dat", 37, 13, 128 * 1024))
		ok = 0;
	puts(ok ? "SD STREAM REOPEN PASS" : "SD STREAM REOPEN FAIL");
	return ok ? 0 : 1;
}
