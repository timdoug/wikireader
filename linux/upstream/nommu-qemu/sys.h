/*
 * Freestanding Linux system calls and console output for the no-MMU
 * reproducers, on RISC-V and m68k. The system call numbers come from the
 * kernel's own UAPI headers (run.sh installs them). The programs are static
 * PIE with no relocations: no libc and no tables of pointers, only
 * PC-relative references.
 */
#ifndef SYS_H
#define SYS_H

#include <asm/unistd.h>

#define AT_FDCWD	-100
#define O_RDONLY	0
#define O_RDWR		2
#define O_SYNC		04010000
#define PROT_READ	1
#define PROT_WRITE	2
#define MAP_SHARED	1
#define MAP_PRIVATE	2
#define MAP_ANONYMOUS	0x20
#define PTRACE_TRACEME	0

#if defined(__riscv)
static inline long syscall6(long n, long a, long b, long c, long d, long e,
			    long f)
{
	register long a0 __asm__("a0") = a;
	register long a1 __asm__("a1") = b;
	register long a2 __asm__("a2") = c;
	register long a3 __asm__("a3") = d;
	register long a4 __asm__("a4") = e;
	register long a5 __asm__("a5") = f;
	register long a7 __asm__("a7") = n;

	__asm__ volatile("ecall" : "+r"(a0)
			 : "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a7)
			 : "memory");
	return a0;
}
#elif defined(__m68k__)
static inline long syscall6(long n, long a, long b, long c, long d, long e,
			    long f)
{
	register long d0 __asm__("d0") = n;
	register long d1 __asm__("d1") = a;
	register long d2 __asm__("d2") = b;
	register long d3 __asm__("d3") = c;
	register long d4 __asm__("d4") = d;
	register long d5 __asm__("d5") = e;
	register long a0 __asm__("a0") = f;

	__asm__ volatile("trap #0" : "+r"(d0)
			 : "r"(d1), "r"(d2), "r"(d3), "r"(d4), "r"(d5), "r"(a0)
			 : "memory");
	return d0;
}
#else
#error "unsupported architecture"
#endif

#define sys(n, a, b, c, d, e, f) \
	syscall6(n, (long)(a), (long)(b), (long)(c), (long)(d), (long)(e), \
		 (long)(f))

static inline long sys_open(const char *path, int flags)
{
	return sys(__NR_openat, AT_FDCWD, path, flags, 0, 0, 0);
}

static inline long sys_mmap(void *addr, unsigned long len, int prot,
			    int flags, int fd, unsigned long off)
{
#ifdef __NR_mmap2
	return sys(__NR_mmap2, addr, len, prot, flags, fd, off >> 12);
#else
	return sys(__NR_mmap, addr, len, prot, flags, fd, off);
#endif
}

static inline __attribute__((noreturn)) void sys_exit(int code)
{
	for (;;)
		sys(__NR_exit, code, 0, 0, 0, 0, 0);
}

static inline int is_err(long r)
{
	return r < 0 && r > -4096;
}

static inline unsigned long str_len(const char *s)
{
	unsigned long n = 0;

	while (s[n])
		n++;
	return n;
}

static inline void put(const char *s)
{
	sys(__NR_write, 1, s, str_len(s), 0, 0, 0);
}

static inline void put_hex(unsigned long v)
{
	char buf[2 + 2 * sizeof(long) + 1];
	int digits = 2 * sizeof(long);
	int i;

	buf[0] = '0';
	buf[1] = 'x';
	for (i = 0; i < digits; i++)
		buf[2 + i] = "0123456789abcdef"[(v >> (4 * (digits - 1 - i))) & 15];
	buf[2 + digits] = 0;
	put(buf);
}

static inline void put_dec(long v)
{
	char buf[21];
	int i = 20;
	int neg = v < 0;
	unsigned long u = neg ? -(unsigned long)v : (unsigned long)v;

	buf[i] = 0;
	do {
		buf[--i] = '0' + u % 10;
		u /= 10;
	} while (u);
	if (neg)
		buf[--i] = '-';
	put(buf + i);
}

/* A system call's result: an address or count, or the errno. */
static inline void put_result(long r)
{
	if (is_err(r)) {
		put("errno ");
		put_dec(-r);
	} else {
		put_hex(r);
	}
}

#endif
