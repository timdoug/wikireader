/* One core, and user code may mask interrupts: the compiler's atomics are
 * inline, a load and a store with interrupts masked around them. */
#define a_cas a_cas
static inline int a_cas(volatile int *p, int t, int s)
{
	__atomic_compare_exchange_n(p, &t, s, 0, __ATOMIC_SEQ_CST,
				    __ATOMIC_SEQ_CST);
	return t;
}

#define a_cas_p a_cas_p
static inline void *a_cas_p(volatile void *p, void *t, void *s)
{
	__atomic_compare_exchange_n((void *volatile *)p, &t, s, 0,
				    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
	return t;
}

#define a_swap a_swap
static inline int a_swap(volatile int *p, int v)
{
	return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST);
}

#define a_fetch_add a_fetch_add
static inline int a_fetch_add(volatile int *p, int v)
{
	return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST);
}

#define a_barrier a_barrier
static inline void a_barrier()
{
	__asm__ __volatile__ ("" : : : "memory");
}

/* A store to a null pointer lands in on-chip RAM, so crash with a trap. */
#define a_crash a_crash
static inline void a_crash()
{
	__asm__ __volatile__ ("int 3" : : : "memory");
	for (;;);
}
