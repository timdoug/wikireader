/* The kernel publishes the running thread's pointer in a word of on-chip
 * RAM at a fixed address (the kernel's asm/tls.h); the compiler reads it
 * from there. */
static inline uintptr_t __get_tp()
{
	return (uintptr_t)__builtin_thread_pointer();
}

/* Variant I: the static TLS blocks follow an 8-byte TCB at the thread
 * pointer, at align_up(8, p_align) for the executable's, as ld lays out
 * local-exec offsets. */
#define TLS_ABOVE_TP
#define GAP_ABOVE_TP 8

#define MC_PC pc

/* Without an MMU nothing protects a guard page: it would only be memory
 * taken from every thread. */
#define DEFAULT_GUARD_SIZE 0
