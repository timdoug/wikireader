// SPDX-License-Identifier: GPL-2.0
#include <string.h>
void crash_in_library(void);

__attribute__((noinline)) static void crash_in_executable(void)
{
	__asm__ volatile("ld.w %%r4,[%0]" : : "r" (0x08000001) : "r4", "memory");
}

int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "library"))
		crash_in_library();
	else
		crash_in_executable();
	return 1;
}
