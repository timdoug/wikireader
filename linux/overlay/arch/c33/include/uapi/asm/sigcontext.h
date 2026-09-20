/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_ASM_C33_SIGCONTEXT_H
#define _UAPI_ASM_C33_SIGCONTEXT_H

struct sigcontext {
	unsigned long r[16];
	unsigned long alr;
	unsigned long ahr;
	unsigned long sp;
	unsigned long psr;
	unsigned long pc;
};

#endif
