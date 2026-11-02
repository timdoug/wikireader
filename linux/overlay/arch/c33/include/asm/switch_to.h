/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_C33_SWITCH_TO_H
#define _ASM_C33_SWITCH_TO_H
struct task_struct;
extern struct task_struct *__c33_switch_to(struct task_struct *prev,
					   struct task_struct *next);
#define switch_to(prev, next, last) \
	((last) = __c33_switch_to((prev), (next)))

/* Where copy_thread() starts a new task: kernel/switch.S. */
void ret_from_fork(void);
void ret_from_kernel_thread(void);
#endif
