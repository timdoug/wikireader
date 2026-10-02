// SPDX-License-Identifier: GPL-2.0-only
/* Map a page of this process's memory through /dev/mem. No-MMU places
 * the mapping at the physical address itself, so it must alias the page,
 * and unmapping the alias must leave the original mapping intact.
 * The top page's mapping would end at 4 GiB, which a VMA cannot represent. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

int main(void)
{
	long size = sysconf(_SC_PAGESIZE);
	int fd = open("/dev/mem", O_RDWR | O_SYNC);
	unsigned char *page = mmap(NULL, size, PROT_READ | PROT_WRITE,
				   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (fd < 0 || page == MAP_FAILED)
		return 1;
	for (long i = 0; i < size; i++)
		page[i] = (unsigned char)(i * 7);

	off_t physical = (off_t)(uintptr_t)page;
	unsigned char *alias = mmap(NULL, size, PROT_READ | PROT_WRITE,
				    MAP_SHARED, fd, physical);
	if (alias != page)
		return 2;
	alias[5] = 0xa5;
	unsigned char byte;
	if (pread(fd, &byte, 1, physical + 5) != 1 || byte != 0xa5)
		return 3;
	if (munmap(alias, size) || page[5] != 0xa5 || page[6] != 42 ||
	    munmap(page, size))
		return 4;

	errno = 0;
	off_t last = (off_t)(0xffffffffUL - size + 1);
	void *top = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, last);
	if (top != MAP_FAILED || errno != ENOMEM) {
		printf("top page: %p errno %d\n", top, errno);
		return 5;
	}
	puts("DEVMEM PASS");
	return 0;
}
