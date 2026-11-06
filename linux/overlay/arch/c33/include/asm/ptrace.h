/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_C33_PTRACE_H
#define _ASM_C33_PTRACE_H
#include <uapi/asm/ptrace.h>

#define C33_PSR_FLAGS	0x0f		/* N, Z, V, C */
#define C33_PSR_IE	(1 << 4)

#ifndef __ASSEMBLY__
/*
 * The exception frame: irq-entry.S builds it at fixed offsets, and its
 * layout is struct user_pt_regs's (kernel/ptrace.c checks).
 */
struct pt_regs {
	unsigned long r[16];
	unsigned long alr;
	unsigned long ahr;
	unsigned long sp;
	unsigned long vector;
	unsigned long orig_r4;
	unsigned long from_user;	/* nonzero: the frame interrupted userspace */
	unsigned long psr;
	unsigned long pc;
};
#endif

#define instruction_pointer(regs) ((regs)->pc)
#define user_stack_pointer(regs) ((regs)->sp)
#define profile_pc(regs) instruction_pointer(regs)
#define user_mode(regs) ((regs)->from_user != 0)
#define interrupts_enabled(regs) ((regs)->psr & C33_PSR_IE)
#define regs_irqs_disabled(regs) (!interrupts_enabled(regs))
#define current_pt_regs() (current->thread.regs)

/*
 * A user frame's PSR, from a value userspace or a tracer supplied: its
 * condition flags, interrupts enabled, interrupt level 0.
 */
#define c33_user_psr(psr) (((psr) & C33_PSR_FLAGS) | C33_PSR_IE)

/*
 * Syscall emulation is a generic-entry feature, but its two requests have no
 * architecture-independent numbers; these are the values every port uses.
 */
#define PTRACE_SYSEMU			0x1f
#define PTRACE_SYSEMU_SINGLESTEP	0x20
#endif
