/*
 * Shared runtime for differential test programs.
 *
 * The same .c file is compiled twice: once by c33-epson-elf-gcc and run in
 * the emulator, once by the host compiler and run natively. Both print the
 * same stream of 8-hex-digit values, and the harness diffs them.
 *
 * Only fixed-width 32-bit types are used. "unsigned int" is 32 bits on both
 * the c33 and on arm64 macOS; "long" is not, so it never appears.
 */
#ifndef DT_H
#define DT_H

typedef unsigned int u32;
typedef int          i32;

#ifdef DT_HOST

#include <stdio.h>
static void emit(u32 v) { printf("%08x\n", v); }

#else

/* EFSIF0, the console the emulator captures. TDBEx is bit 1 of STATUS. */
#define DT_TXD    (*(volatile unsigned char *)0x00300b00)
#define DT_STATUS (*(volatile unsigned char *)0x00300b02)

static void dt_putc(int c)
{
	while (!(DT_STATUS & 2))
		;
	DT_TXD = (unsigned char)c;
}

static void emit(u32 v)
{
	static const char hex[] = "0123456789abcdef";
	int i;
	for (i = 28; i >= 0; i -= 4)
		dt_putc(hex[(v >> i) & 15]);
	dt_putc('\n');
}

#endif

/* Guards that keep the generated programs free of undefined behaviour. */
#define SH(n)     ((n) & 31)                       /* shift count in range */
#define DIVU(a,b) ((b) ? (a) / (b) : (a))          /* no divide by zero */
#define MODU(a,b) ((b) ? (a) % (b) : (a))
/* INT_MIN / -1 overflows, so fold that one case away. */
#define DIVS(a,b) (((b) == 0 || ((a) == (i32)0x80000000 && (b) == -1)) ? (a) : (a) / (b))
#define MODS(a,b) (((b) == 0 || ((a) == (i32)0x80000000 && (b) == -1)) ? (a) : (a) % (b))

#endif /* DT_H */
