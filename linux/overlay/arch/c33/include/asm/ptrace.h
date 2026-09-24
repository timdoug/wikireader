/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_C33_PTRACE_H
#define _ASM_C33_PTRACE_H
#include <uapi/asm/ptrace.h>
#define instruction_pointer(regs) ((regs)->pc)
#define user_stack_pointer(regs) ((regs)->sp)
#define profile_pc(regs) instruction_pointer(regs)
#define user_mode(regs) ((regs)->reserved != 0)
#define interrupts_enabled(regs) ((regs)->psr & (1 << 4))
#define regs_irqs_disabled(regs) (!interrupts_enabled(regs))
#define current_pt_regs() (current->thread.regs)

/*
 * Syscall emulation is a generic-entry feature, but its two requests have no
 * architecture-independent numbers; these are the values every port uses.
 */
#define PTRACE_SYSEMU			0x1f
#define PTRACE_SYSEMU_SINGLESTEP	0x20
#endif
