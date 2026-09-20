/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_C33_CURRENT_H
#define _ASM_C33_CURRENT_H
struct task_struct;
extern struct task_struct *c33_current_task;
#define current c33_current_task
register unsigned long current_stack_pointer __asm__("sp");
#endif
