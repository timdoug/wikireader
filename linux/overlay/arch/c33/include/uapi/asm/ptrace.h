/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_ASM_C33_PTRACE_H
#define _UAPI_ASM_C33_PTRACE_H

struct pt_regs {
	unsigned long r[16];
	unsigned long alr;
	unsigned long ahr;
	unsigned long sp;
	unsigned long vector;
	unsigned long orig_r4;
	unsigned long reserved;
	unsigned long psr;
	unsigned long pc;
};

/* FDPIC: PTRACE_GETFDPIC reads the address of a process's load map, its own
   (PTRACE_GETFDPIC_EXEC) or its interpreter's (PTRACE_GETFDPIC_INTERP).
   31 and 32 are PTRACE_SYSEMU and PTRACE_SYSEMU_SINGLESTEP.  */
#define PTRACE_GETFDPIC		33
#define PTRACE_GETFDPIC_EXEC	0
#define PTRACE_GETFDPIC_INTERP	1

/* Read/write the TLS base as a word at DATA; ADDR must be zero. */
#define PTRACE_GET_THREAD_AREA	25
#define PTRACE_SET_THREAD_AREA	26

#endif
