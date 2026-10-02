// SPDX-License-Identifier: GPL-2.0-only
/* Build with both DT_RPATH and DT_RUNPATH set to $ORIGIN. The app runner
 * executes this on the card from a different working directory. */
#include <dlfcn.h>
#include <stdio.h>

int main(void)
{
	void *library = dlopen("origin.so", RTLD_NOW);
	if (!library) {
		puts(dlerror());
		return 1;
	}
	int (*value)(void) = dlsym(library, "origin_value");
	if (!value || value() != 42 || dlclose(library))
		return 1;
	puts("ORIGIN PASS");
	return 0;
}
