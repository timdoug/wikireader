/* divref.c - what guest/divtest.c should print, computed natively.  The
   semantics are RISC-V's: a zero divisor gives all ones and the dividend, the
   overflowing signed divide gives the dividend and zero. */
#include <stdint.h>
#include <stdio.h>

#include "../guest/divtable.h"

static uint32_t rv_div(uint32_t a, uint32_t b)
{
	if (b == 0) return 0xffffffffu;
	if (a == 0x80000000u && b == 0xffffffffu) return a;
	return (uint32_t)((int32_t)a / (int32_t)b);
}
static uint32_t rv_rem(uint32_t a, uint32_t b)
{
	if (b == 0) return a;
	if (a == 0x80000000u && b == 0xffffffffu) return 0;
	return (uint32_t)((int32_t)a % (int32_t)b);
}

int main(void)
{
	for (unsigned k = 0; k < 4; ++k) {
		uint32_t h = k;

		for (unsigned i = 0; i < NTABLE; ++i)
			for (unsigned j = 0; j < NTABLE; ++j) {
				uint32_t a = table[i], b = table[j];

				h = h * 31 + rv_div(a, b);
				h = h * 31 + rv_rem(a, b);
				h = h * 31 + (b ? a / b : 0xffffffffu);
				h = h * 31 + (b ? a % b : a);
				h = h * 31 + a * b;
				h = h * 31 + (uint32_t)(((int64_t)(int32_t)a * (int64_t)(int32_t)b) >> 32);
				h = h * 31 + (uint32_t)(((uint64_t)a * (uint64_t)b) >> 32);
				h = h * 31 + (uint32_t)(((int64_t)(int32_t)a * (int64_t)(uint64_t)b) >> 32);
			}
		printf("round 0x%08x\n", h);
	}
	return 0;
}
