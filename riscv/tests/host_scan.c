/* host_scan.c - translate a guest image on the build machine, region by
 * region, and say what the translator made of each: how much code, and how
 * many loops it could check at the entry and lay out to fit the fetch
 * window.
 *
 *   make jitscan IMAGE=build/rvbench.bin AT="80000148 800001a8"
 *
 * Every region is translated from the address given, as the runtime would
 * translate it on being asked, so the question a run cannot answer -- why
 * was that loop not hoisted? -- is answered here in a second, with a
 * debugger to hand.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../rv32.h"
#include "../rv32_jit.h"
void rv32_jit_stub_fault(void) {}
void rv32_jit_stub_indirect(void) {}
void rv32_jit_stub_dev_load(void) {}
void rv32_jit_stub_dev_store(void) {}
void rv32_jit_stub_divop(void) {}
void rv32_jit_stub_csr(void) {}
void rv32_jit_stub_amo(void) {}
#include "../rv32_jit.c"
int main(int argc, char **argv)
{
	static uint8_t arena[2 * 1024 * 1024];
	static uint8_t ram[256 * 1024];
	rv32_t s;
	FILE *f = fopen(argv[1], "rb");
	size_t n = fread(ram, 1, sizeof ram, f);
	int i;
	memset(&s, 0, sizeof s);
	s.ram = ram; s.ram_size = sizeof ram; s.reservation = 0xffffffffu;
	rv32_jit_init(arena, sizeof arena);
	printf("%zu bytes\n", n);
	for (i = 2; i < argc; ++i) {
		uint32_t pc = strtoul(argv[i], 0, 16);
		uint32_t l0 = rv32_jit.loops, r0 = rv32_jit.resident, b0 = rv32_jit.bytes;
		int full;
		uint8_t *code = translate(&s, pc, &full);
		printf("%08x: %s, %u bytes, loops %u resident %u\n", pc, code ? "ok" : "no",
		       rv32_jit.bytes - b0, rv32_jit.loops - l0, rv32_jit.resident - r0);
	}
	return 0;
}
