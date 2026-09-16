/* ref.c - run the guest kernels natively and print their checksums.
 *
 * The interpreter is only trustworthy if the numbers it produces match the
 * same C compiled for the build machine.  Any divergence here is a bug in
 * rv32.c, not in the benchmark.
 */

#include <stdint.h>
#include <stdio.h>

static uint32_t uart_sink, mark_sink;
#define UART (&uart_sink)
#define MARK (&mark_sink)
#define RV_BENCH_NO_MAIN

/* The linker symbol the guest image uses for its working area. */
_Alignas(16) char __heap_start[1024 * 1024];

#include "../guest/bench.c"

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

	for (unsigned k = 0; k < sizeof kernels / sizeof kernels[0]; ++k)
		printf("%s 0x%08x\n", kernels[k].name, kernels[k].run());
	(void)puts_;
	(void)puthex;
	(void)putu;
	return 0;
}
