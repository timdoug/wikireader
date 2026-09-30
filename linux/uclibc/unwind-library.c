/* A reloadable C frame through which the C++ test throws. */
void unwind_library(void (*callback)(void))
{
	callback();
	/* Keep this frame: a tail call would skip its unwind table. */
	__asm__ volatile("" ::: "memory");
}
