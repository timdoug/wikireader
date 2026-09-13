/* Zstandard's standard allocation hooks, mapped to the Grifo application heap. */
#include <grifo.h>
#include <regs.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void *malloc(size_t size)
{
	return size ? memory_allocate(size, "zim-heap") : NULL;
}

void *calloc(size_t count, size_t size)
{
	void *memory;
	if (count && size > (size_t)-1 / count)
		return NULL;
	memory = malloc(count * size);
	if (memory)
		memset(memory, 0, count * size);
	return memory;
}

void free(void *memory)
{
	if (memory)
		memory_free(memory, "zim-heap");
}
