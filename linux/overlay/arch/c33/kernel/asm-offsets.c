// SPDX-License-Identifier: GPL-2.0
#define COMPILE_OFFSETS
#include <linux/kbuild.h>
#include <linux/sched.h>

int main(void)
{
	DEFINE(TI_FLAGS, offsetof(struct thread_info, flags));
	DEFINE(TASK_THREAD, offsetof(struct task_struct, thread));
	DEFINE(THREAD_KSP, offsetof(struct thread_struct, ksp));
	return 0;
}
