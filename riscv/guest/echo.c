/* echo.c - a guest that proves the console works in both directions.
 *
 * Reads the 8250's line status, echoes whatever arrives, and powers off on
 * 'q'.  Boots in a moment, which is what makes it useful: the Linux image
 * takes half an hour of wall clock to reach a prompt, and the input path is
 * worth testing without waiting for that.
 */

#include <stdint.h>

#define UART ((volatile uint32_t *)0x10000000)
#define LSR  ((volatile uint32_t *)0x10000005)
#define SYS  ((volatile uint32_t *)0x11100000)

static void putc_(char c)
{
	*UART = (uint8_t)c;
}

static void puts_(const char *s)
{
	while (*s)
		putc_(*s++);
}

int main(void)
{
	puts_("\033[1;34mcolour\033[m plain\n");
	puts_("echo: type, q quits\n> ");
	for (;;) {
		if (!(*LSR & 1))
			continue;
		uint32_t c = *UART & 0xff;
		if (c == 'q') {
			puts_("\nbye\n");
			*SYS = 0x5555;
			for (;;) {}
		}
		if (c == '\r' || c == '\n')
			puts_("\n> ");
		else
			putc_((char)c);
	}
}
