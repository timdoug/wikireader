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

#endif
