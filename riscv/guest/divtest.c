/* divtest.c - every M-extension operation over a table of awkward operands.
 *
 * The divide is hand-written C33 assembly (rv32_div.s) with RISC-V's answers
 * for a zero divisor and the one overflowing signed case built in, and rvbench
 * only exercises the shapes its kernel happens to produce.  This runs all
 * pairs from a table of edge values through div, divu, rem, remu and the
 * four multiplies, folds the answers into one word a round, and prints it.
 * tests/divref.c computes the same word natively.  Several rounds, so that
 * the first is mostly interpreted and the rest translated: the interpreter
 * and the translator reach the divide by different entries.
 */

#include <stdint.h>

#define UART ((volatile uint32_t *)0x10000000)
#define SYS  ((volatile uint32_t *)0x11100000)

static void putc_(char c) { *UART = (uint8_t)c; }
static void puts_(const char *s) { while (*s) putc_(*s++); }
static void puthex(uint32_t v)
{
	puts_("0x");
	for (int i = 28; i >= 0; i -= 4)
		putc_("0123456789abcdef"[(v >> i) & 15]);
}

#include "divtable.h"

static uint32_t round_(uint32_t seed)
{
	uint32_t h = seed;
	volatile uint32_t *tab = (volatile uint32_t *)table;

	for (unsigned i = 0; i < NTABLE; ++i)
		for (unsigned j = 0; j < NTABLE; ++j) {
			uint32_t a = tab[i], b = tab[j];
			uint32_t q, r, qu, ru, m, mh, mhu, mhsu;

			__asm__("div %0,%1,%2" : "=r"(q) : "r"(a), "r"(b));
			__asm__("rem %0,%1,%2" : "=r"(r) : "r"(a), "r"(b));
			__asm__("divu %0,%1,%2" : "=r"(qu) : "r"(a), "r"(b));
			__asm__("remu %0,%1,%2" : "=r"(ru) : "r"(a), "r"(b));
			__asm__("mul %0,%1,%2" : "=r"(m) : "r"(a), "r"(b));
			__asm__("mulh %0,%1,%2" : "=r"(mh) : "r"(a), "r"(b));
			__asm__("mulhu %0,%1,%2" : "=r"(mhu) : "r"(a), "r"(b));
			__asm__("mulhsu %0,%1,%2" : "=r"(mhsu) : "r"(a), "r"(b));
			h = h * 31 + q;
			h = h * 31 + r;
			h = h * 31 + qu;
			h = h * 31 + ru;
			h = h * 31 + m;
			h = h * 31 + mh;
			h = h * 31 + mhu;
			h = h * 31 + mhsu;
		}
	return h;
}

int main(void)
{
	puts_("divtest\n");
	for (unsigned k = 0; k < 4; ++k) {
		puts_("round ");
		puthex(round_(k));
		putc_('\n');
	}
	puts_("done\n");
	*SYS = 0x5555;
	for (;;) {}
}
