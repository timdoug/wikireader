/* bench.c - the rv32ima workload whose cost per instruction is the whole
 * point of this first step.
 *
 * Each kernel isolates one thing the interpreter has to do well: register
 * arithmetic, taken branches, the multiply and divide it has to synthesise,
 * and the three shapes of guest memory access.  Every kernel prints a
 * checksum, so a run that reports a plausible speed but a wrong number is
 * caught immediately, and brackets itself with a marker write so the host
 * can attribute cycles without the guest needing a clock.
 */

#include <stdint.h>

/* The reference build compiles this same file for the build machine to
   check the checksums, and supplies its own device addresses and heap. */
#ifndef UART
#define UART  ((volatile uint32_t *)0x10000000)
#define MARK  ((volatile uint32_t *)0x11200000)
#endif

static void putc_(char c)
{
	*UART = (uint8_t)c;
}

static void puts_(const char *s)
{
	while (*s)
		putc_(*s++);
}

static void putu(uint32_t v)
{
	char buf[12];
	int n = 0;
	if (!v) {
		putc_('0');
		return;
	}
	while (v) {
		buf[n++] = '0' + (char)(v % 10);
		v /= 10;
	}
	while (n)
		putc_(buf[--n]);
}

static void puthex(uint32_t v)
{
	puts_("0x");
	for (int shift = 28; shift >= 0; shift -= 4)
		putc_("0123456789abcdef"[(v >> shift) & 15]);
}

/* Everything below scales with this.  The default sizes the whole run at
   about 1.2 million guest instructions, which the full-system emulator gets
   through in well under a minute; SCALE=16 is the long form for when a
   difference is small enough to be worth the wait. */
#ifndef SCALE
#define SCALE 1
#endif

extern char __heap_start[];

static uint32_t *const work = (uint32_t *)__heap_start;
/* The C33 has no data cache, so what this has to be large enough to cover
   is SDRAM rows -- 1 KiB each -- not a cache. */
#define WORDS (8192 * SCALE)

static uint32_t k_alu(void)
{
	uint32_t a = 1, b = 2, c = 3, d = 4;
	for (int i = 0; i < 4000 * SCALE; ++i) {
		a += b ^ c;
		b += a >> 3;
		c ^= a + d;
		d += c << 1;
	}
	return a ^ b ^ c ^ d;
}

static uint32_t k_branch(void)
{
	uint32_t seed = 12345, hits = 0;
	for (int i = 0; i < 6000 * SCALE; ++i) {
		seed = seed * 1103515245u + 12345u;
		if (seed & 0x00010000u)
			hits += 1;
		if ((int32_t)seed < 0)
			hits += 2;
		else if (seed % 3 == 0)
			hits += 3;
	}
	return hits;
}

static uint32_t k_mul(void)
{
	uint32_t a = 1234567, s = 0;
	for (int i = 0; i < 4000 * SCALE; ++i) {
		a = a * 2654435761u + 1;
		s += a * (uint32_t)i;
		/* mulh and mulhu as well as mul: the C33's multiply produces
		   the high word anyway, so all three are worth checking. */
		s += (uint32_t)(((int64_t)(int32_t)a * (int64_t)(int32_t)i) >> 32);
		s += (uint32_t)(((uint64_t)a * (uint64_t)i) >> 32);
	}
	return s;
}

static uint32_t k_div(void)
{
	uint32_t s = 0, a = 0x12345678;
	for (int i = 1; i < 2000 * SCALE; ++i) {
		s += a / (uint32_t)i;
		s += a % (uint32_t)(i + 7);
		/* signed too: div and rem round toward zero, unlike the
		   unsigned pair, and have their own overflow case */
		s += (uint32_t)((int32_t)a / -i);
		s += (uint32_t)((int32_t)a % -(i + 3));
		a += 0x9e3779b9u;
	}
	return s;
}

static uint32_t k_load(void)
{
	uint32_t s = 0;
	for (int pass = 0; pass < 2; ++pass)
		for (int i = 0; i < WORDS; ++i)
			s += work[i];
	return s;
}

static uint32_t k_store(void)
{
	for (int pass = 0; pass < 2; ++pass)
		for (int i = 0; i < WORDS; ++i)
			work[i] = (uint32_t)i + (uint32_t)pass;
	return work[WORDS - 1];
}

static uint32_t k_copy(void)
{
	uint32_t *dst = work + WORDS / 2;
	for (int pass = 0; pass < 4; ++pass)
		for (int i = 0; i < WORDS / 2; ++i)
			dst[i] = work[i];
	return dst[WORDS / 2 - 1];
}

static uint32_t k_bytes(void)
{
	uint8_t *p = (uint8_t *)work;
	uint32_t s = 0;
	for (int pass = 0; pass < 1; ++pass)
		for (int i = 0; i < WORDS * 4; ++i)
			s = s * 31u + p[i];
	return s;
}

static uint32_t crc_table[256];

static uint32_t k_crc(void)
{
	const uint8_t *p = (const uint8_t *)work;
	uint32_t crc = 0xffffffffu;
	for (int pass = 0; pass < 1; ++pass)
		for (int i = 0; i < WORDS * 4; ++i)
			crc = crc_table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
	return ~crc;
}

static uint32_t k_sieve(void)
{
	uint8_t *flags = (uint8_t *)work;
	const int n = 20000 * SCALE;
	uint32_t count = 0;
	for (int pass = 0; pass < 1; ++pass) {
		for (int i = 0; i < n; ++i)
			flags[i] = 1;
		count = 0;
		for (int i = 2; i < n; ++i) {
			if (!flags[i])
				continue;
			++count;
			for (int j = i + i; j < n; j += i)
				flags[j] = 0;
		}
	}
	return count;
}

struct kernel {
	const char *name;
	uint32_t (*run)(void);
	uint32_t expect;   /* filled in once, from the host reference run */
};

static const struct kernel kernels[] = {
	{ "alu",    k_alu,    0 },
	{ "branch", k_branch, 0 },
	{ "mul",    k_mul,    0 },
	{ "div",    k_div,    0 },
	{ "load",   k_load,   0 },
	{ "store",  k_store,  0 },
	{ "copy",   k_copy,   0 },
	{ "bytes",  k_bytes,  0 },
	{ "crc",    k_crc,    0 },
	{ "sieve",  k_sieve,  0 },
};

#ifndef RV_BENCH_NO_MAIN
int main(void)
{
	for (uint32_t i = 0; i < 256; ++i) {
		uint32_t c = i;
		for (int bit = 0; bit < 8; ++bit)
			c = (c & 1) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
		crc_table[i] = c;
	}
	for (int i = 0; i < WORDS; ++i)
		work[i] = (uint32_t)i * 2654435761u;

	puts_("rv32 bench\n");
	for (unsigned k = 0; k < sizeof kernels / sizeof kernels[0]; ++k) {
		*MARK = k + 1;
		uint32_t result = kernels[k].run();
		puts_(kernels[k].name);
		puts_(" ");
		puthex(result);
		putc_('\n');
	}
	*MARK = 0;
	puts_("done\n");
	(void)putu;
	return 0;
}
#endif
