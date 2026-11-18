/*
 * Maps one anonymous page, fills it with "HOLDER", writes the page's
 * address to fd 4, and exits after a byte arrives on fd 3.
 */
#include "sys.h"

void start_main(long *sp)
{
	long p = sys_mmap(0, 4096, PROT_READ | PROT_WRITE,
			  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	char c;

	(void)sp;
	if (is_err(p))
		sys_exit(1);
	((char *)p)[0] = 'H';
	((char *)p)[1] = 'O';
	((char *)p)[2] = 'L';
	((char *)p)[3] = 'D';
	((char *)p)[4] = 'E';
	((char *)p)[5] = 'R';
	sys(__NR_write, 4, &p, sizeof(p), 0, 0, 0);
	sys(__NR_read, 3, &c, 1, 0, 0, 0);
	sys_exit(0);
}
