/* Exercise all shared-library TLS models supported by the C33 ELF ABI. */
#define _GNU_SOURCE
#include <stdint.h>

__thread int library_ie __attribute__((tls_model("initial-exec"))) = 71;
__thread int library_gd __attribute__((tls_model("global-dynamic"))) = 72;
static __thread int library_ld __attribute__((tls_model("local-dynamic"))) = 73;
static __thread volatile unsigned char aligned_tls[64] __attribute__((aligned(32)));

int library_tls(int seed)
{
	if (seed < 0)
		return library_ie + library_gd + library_ld;
	int initial = library_ie == 71 && library_gd == 72 && library_ld == 73;
	uintptr_t address = (uintptr_t)aligned_tls;
	__asm__("" : "+r"(address));
	if (address & 31)
		return -1;
	library_ie = seed;
	library_gd = seed + 1;
	library_ld = seed + 2;
	aligned_tls[63] = seed;
	return initial;
}
