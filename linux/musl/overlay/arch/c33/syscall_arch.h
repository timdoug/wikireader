/* int 0 with the number in %r4 and up to six arguments in %r6-%r11; the
 * result comes back in %r4.  A 64-bit argument takes the next two words,
 * low first, with no alignment. */
#define __SYSCALL_LL_E(x) \
((union { long long ll; long l[2]; }){ .ll = x }).l[0], \
((union { long long ll; long l[2]; }){ .ll = x }).l[1]
#define __SYSCALL_LL_O(x) __SYSCALL_LL_E((x))

#define __asm_syscall(...) do { \
	__asm__ __volatile__ ( "int 0" \
	: "+r"(r4) : __VA_ARGS__ : "memory"); \
	return r4; \
	} while (0)

static inline long __syscall0(long n)
{
	register long r4 __asm__("r4") = n;
	__asm_syscall();
}

static inline long __syscall1(long n, long a)
{
	register long r4 __asm__("r4") = n;
	register long r6 __asm__("r6") = a;
	__asm_syscall("r"(r6));
}

static inline long __syscall2(long n, long a, long b)
{
	register long r4 __asm__("r4") = n;
	register long r6 __asm__("r6") = a;
	register long r7 __asm__("r7") = b;
	__asm_syscall("r"(r6), "r"(r7));
}

static inline long __syscall3(long n, long a, long b, long c)
{
	register long r4 __asm__("r4") = n;
	register long r6 __asm__("r6") = a;
	register long r7 __asm__("r7") = b;
	register long r8 __asm__("r8") = c;
	__asm_syscall("r"(r6), "r"(r7), "r"(r8));
}

static inline long __syscall4(long n, long a, long b, long c, long d)
{
	register long r4 __asm__("r4") = n;
	register long r6 __asm__("r6") = a;
	register long r7 __asm__("r7") = b;
	register long r8 __asm__("r8") = c;
	register long r9 __asm__("r9") = d;
	__asm_syscall("r"(r6), "r"(r7), "r"(r8), "r"(r9));
}

static inline long __syscall5(long n, long a, long b, long c, long d, long e)
{
	register long r4 __asm__("r4") = n;
	register long r6 __asm__("r6") = a;
	register long r7 __asm__("r7") = b;
	register long r8 __asm__("r8") = c;
	register long r9 __asm__("r9") = d;
	register long r10 __asm__("r10") = e;
	__asm_syscall("r"(r6), "r"(r7), "r"(r8), "r"(r9), "r"(r10));
}

static inline long __syscall6(long n, long a, long b, long c, long d, long e, long f)
{
	register long r4 __asm__("r4") = n;
	register long r6 __asm__("r6") = a;
	register long r7 __asm__("r7") = b;
	register long r8 __asm__("r8") = c;
	register long r9 __asm__("r9") = d;
	register long r10 __asm__("r10") = e;
	register long r11 __asm__("r11") = f;
	__asm_syscall("r"(r6), "r"(r7), "r"(r8), "r"(r9), "r"(r10), "r"(r11));
}

/* The generic IPC calls take no IPC_64 flag. */
#define IPC_64 0
