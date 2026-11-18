/*
 * Prints where the initial stack image lies against the stack mapping.
 * The kernel copies the argument and environment strings to the top of
 * the stack allocation, so the mapping that contains the highest string
 * is the stack.
 *
 * Built several times with different -z stack-size values. Without
 * arguments, prints the stack pointer and the mapping bounds. With a
 * decimal argument, the PT_GNU_STACK size of this build, prints nothing
 * and exits with:
 *
 *   0  the image is inside the mapping, and the room below the stack
 *      pointer is at least min(size - image, size - size / 4), where size
 *      is the PT_GNU_STACK size raised to two pages and rounded up to a
 *      page (AT_PAGESZ)
 *   1  the stack pointer is below the mapping
 *   2  the strings are outside every mapping
 *   3  the image is inside the mapping, with less room than above
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

static unsigned long parse_dec(const char *s)
{
	unsigned long v = 0;

	while (*s >= '0' && *s <= '9')
		v = v * 10 + *s++ - '0';
	return v;
}

static unsigned long string_end(char **v, unsigned long top)
{
	for (; *v; v++) {
		unsigned long e = (unsigned long)*v + str_len(*v) + 1;

		if (e > top)
			top = e;
	}
	return top;
}

void start_main(long *sp)
{
	long argc = sp[0];
	char **argv = (char **)(sp + 1);
	char **envp = argv + argc + 1;
	unsigned long top, start = 0, end = 0;
	unsigned long size, image, room, want, page = 0;
	unsigned long *aux;
	const char *s;
	long fd, n = 0, r;

	top = string_end(envp, string_end(argv, 0));
	/* The auxiliary vector follows envp's NULL; AT_PAGESZ is 6. */
	for (aux = (unsigned long *)envp; *aux; aux++)
		;
	for (aux++; aux[0]; aux += 2)
		if (aux[0] == 6)
			page = aux[1];

	fd = sys_open("/proc/self/maps", O_RDONLY);
	while (fd >= 0 && n < (long)sizeof(maps) - 1 &&
	       (r = sys(__NR_read, fd, maps + n, sizeof(maps) - 1 - n,
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

	if (argc > 1 && argv[1][0] >= '0' && argv[1][0] <= '9') {
		if (!end)
			sys_exit(2);
		if ((unsigned long)sp < start)
			sys_exit(1);
		/* The stack size the loader starts from: two pages at least. */
		size = parse_dec(argv[1]);
		if (!size)
			size = 131072;
		if (size < 2 * page)
			size = 2 * page;
		size = (size + page - 1) & ~(page - 1);
		image = end - (unsigned long)sp;
		room = (unsigned long)sp - start;
		want = size - size / 4;
		if (size > image && size - image < want)
			want = size - image;
		sys_exit(room >= want ? 0 : 3);
	}

	put("/stack-victim: strings end at ");
	put_hex(top);
	put("\n/stack-victim: stack mapping ");
	put_hex(start);
	put("-");
	put_hex(end);
	put(", initial sp ");
	put_hex((unsigned long)sp);
	put("\n");
	if (!end)
		put("/stack-victim: the strings are outside every mapping\n");
	else if ((unsigned long)sp < start)
		put("/stack-victim: initial sp is BELOW the stack mapping\n");
	else
		put("/stack-victim: initial sp is inside the stack mapping\n");
	sys_exit(0);
}
