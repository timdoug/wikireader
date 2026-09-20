#ifndef _BITS_SYSCALLS_H
#define _BITS_SYSCALLS_H

#ifndef _SYSCALL_H
# error "Never use <bits/syscalls.h> directly; include <sys/syscall.h> instead."
#endif

#ifdef __ASSEMBLER__

# undef DO_CALL
# define DO_CALL(syscall_name, args) \
	xld.w %r4,SYS_ify(syscall_name); \
	int 0

#else

# define INTERNAL_SYSCALL_NCS(name, err, nr, args...) \
  ({ \
	LOAD_ARGS_##nr(args) \
	register long __result __asm__("r4") = (long)(name); \
	__asm__ volatile ("int 0" \
		: "+r" (__result) \
		: ASM_ARGS_##nr \
		: "memory"); \
	__result; \
  })

# define LOAD_ARGS_0()
# define ASM_ARGS_0

# define LOAD_ARGS_1(a1) \
	register long __a1 __asm__("r6") = (long)(a1);
# define ASM_ARGS_1 "r" (__a1)

# define LOAD_ARGS_2(a1, a2) \
	LOAD_ARGS_1(a1) \
	register long __a2 __asm__("r7") = (long)(a2);
# define ASM_ARGS_2 ASM_ARGS_1, "r" (__a2)

# define LOAD_ARGS_3(a1, a2, a3) \
	LOAD_ARGS_2(a1, a2) \
	register long __a3 __asm__("r8") = (long)(a3);
# define ASM_ARGS_3 ASM_ARGS_2, "r" (__a3)

# define LOAD_ARGS_4(a1, a2, a3, a4) \
	LOAD_ARGS_3(a1, a2, a3) \
	register long __a4 __asm__("r9") = (long)(a4);
# define ASM_ARGS_4 ASM_ARGS_3, "r" (__a4)

# define LOAD_ARGS_5(a1, a2, a3, a4, a5) \
	LOAD_ARGS_4(a1, a2, a3, a4) \
	register long __a5 __asm__("r10") = (long)(a5);
# define ASM_ARGS_5 ASM_ARGS_4, "r" (__a5)

# define LOAD_ARGS_6(a1, a2, a3, a4, a5, a6) \
	LOAD_ARGS_5(a1, a2, a3, a4, a5) \
	register long __a6 __asm__("r11") = (long)(a6);
# define ASM_ARGS_6 ASM_ARGS_5, "r" (__a6)

#endif
#endif
