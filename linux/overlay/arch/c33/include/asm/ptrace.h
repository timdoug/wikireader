/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_C33_PTRACE_H
#define _ASM_C33_PTRACE_H
#include <uapi/asm/ptrace.h>
#define instruction_pointer(regs) ((regs)->pc)
#define user_stack_pointer(regs) ((regs)->sp)
#define profile_pc(regs) instruction_pointer(regs)
#define user_mode(regs) (1)
#define interrupts_enabled(regs) ((regs)->psr & (1 << 4))
#define current_pt_regs() (current->thread.regs)
#endif
