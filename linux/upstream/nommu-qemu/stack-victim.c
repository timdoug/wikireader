/*
 * Linked with -z stack-size=8192, so the PT_GNU_STACK size is 8 KiB.
 * Prints the initial stack pointer and the bounds of the mapping that
 * contains the environment strings, which the kernel copies to the top of
 * the stack allocation.
 */
#include "sys.h"

static char maps[8192];

static unsigned long parse_hex(const char **s)
{
	unsigned long v = 0;

	for (;;) {
		char c = **s;

		if (c >= '0' && c <= '9')
			v = v * 16 + c - '0';
		else if (c >= 'a' && c <= 'f')
			v = v * 16 + c - 'a' + 10;
		else
			return v;
		(*s)++;
	}
}

void start_main(long *sp)
{
	long argc = sp[0];
	char **envp = (char **)(sp + 1) + argc + 1;
	unsigned long top = 0, start = 0, end = 0;
	const char *s;
	long fd, n = 0, r;
	int i;

	for (i = 0; envp[i]; i++) {
		unsigned long e = (unsigned long)envp[i] + str_len(envp[i]) + 1;

		if (e > top)
			top = e;
	}

	fd = sys_open("/proc/self/maps", O_RDONLY);
	while (fd >= 0 && n < (long)sizeof(maps) - 1 &&
	       (r = sys(SYS_read, fd, maps + n, sizeof(maps) - 1 - n,
			0, 0, 0)) > 0)
		n += r;
	maps[n > 0 ? n : 0] = 0;

	for (s = maps; *s; ) {
		unsigned long a = parse_hex(&s), b;

		if (*s++ != '-')
			break;
		b = parse_hex(&s);
		if (a < top && top <= b) {
			start = a;
			end = b;
		}
		while (*s && *s++ != '\n')
			;
	}

	put("/stack-victim: ");
	put_dec(i);
	put(" environment strings ending at ");
	put_hex(top);
	put("\n/stack-victim: stack mapping ");
	put_hex(start);
	put("-");
	put_hex(end);
	put(", initial sp ");
	put_hex((unsigned long)sp);
	put("\n");
	if (!end)
		put("/stack-victim: the environment is outside every mapping\n");
	else if ((unsigned long)sp < start)
		put("/stack-victim: initial sp is BELOW the stack mapping\n");
	else
		put("/stack-victim: initial sp is inside the stack mapping\n");
	sys_exit(0);
}
