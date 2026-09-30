/* Loaded after workers start: exercises DTV growth and dynamic TLS. */
#include <stdint.h>

static __thread volatile unsigned char bytes[64]
	__attribute__((aligned(64), tls_model("local-dynamic")));
static __thread int value __attribute__((tls_model("local-dynamic")));

int late_tls(int seed)
{
	if (seed < 0)
		return value;
	uintptr_t address = (uintptr_t)bytes;
	__asm__ ("" : "+r"(address));
	int pristine = value == 0 && bytes[63] == 0 && (address & 63) == 0;
	value = seed;
	bytes[63] = seed;
	return pristine;
}
