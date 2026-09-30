// SPDX-License-Identifier: GPL-2.0
/* Alignment exceptions are real SIGBUS faults on both hardware and wremu;
 * unlike a NULL load they do not depend on an MMU. */
__attribute__((noinline)) void crash_in_library(void)
{
	__asm__ volatile("ld.w %%r4,[%0]" : : "r" (0x08000001) : "r4", "memory");
}
