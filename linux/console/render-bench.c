// SPDX-License-Identifier: GPL-2.0-only
/* C33 process CPU time for terminal rendering, without PTY I/O, panel writes
 * or animation pacing. Boot through Grifo and run this from the Linux shell.
 */
#define main wr_console_main
#include "wr-console.c"
#undef main

static long long cpu_ns(void)
{
	struct timespec time;

	if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &time))
		exit(2);
	return (long long)time.tv_sec * 1000000000 + time.tv_nsec;
}

int main(void)
{
	static const struct {
		const char *name, *line;
		unsigned int iterations;
	} cases[] = {
		{ "ascii_scroll", "The quick brown fox: 0123456789 ABCDEFGH\r\n", 2000 },
		{ "unicode_scroll", "caf\xc3\xa9 \xce\xbb \xd0\x96 \xe2\x82\xac e\xcc\x81 "
		  "\xe6\x97\xa5 \xf0\x9f\x98\x80 0123456789 ABCDEFGH\r\n", 2000 },
	};

	if (!setlocale(LC_CTYPE, "C.UTF-8"))
		return 1;
	for (unsigned int c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
		for (unsigned int trial = 0; trial < 3; trial++) {
			long long start, elapsed;
			uint32_t checksum = 0;

			terminal_reset();
			start = cpu_ns();
			for (unsigned int i = 0; i < cases[c].iterations; i++)
				for (const unsigned char *p = (const unsigned char *)cases[c].line; *p; p++)
					terminal_byte(*p);
			elapsed = cpu_ns() - start;
			for (unsigned int i = 0; i < LCD_BYTES; i++)
				checksum = checksum * 33 + framebuffer[i];
			printf("RENDER %s bytes=%u ns=%lld checksum=%08x\n", cases[c].name,
			       (unsigned int)(cases[c].iterations * strlen(cases[c].line)),
			       elapsed, checksum);
		}
	}
	return 0;
}
