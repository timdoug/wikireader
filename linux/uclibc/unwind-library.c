/* A reloadable C frame through which the C++ test throws. */
void unwind_library(void (*callback)(void))
{
	/* Force the highest preserved register into the save block, checking
	 * its restoration when the callback throws or cancels a thread. */
	__asm__ volatile("ld.w %%r3,7" ::: "r3");
	callback();
	/* Keep this frame: a tail call would skip its unwind table. */
	__asm__ volatile("" ::: "memory");
}
