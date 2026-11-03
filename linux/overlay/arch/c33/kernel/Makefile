# SPDX-License-Identifier: GPL-2.0
always-$(KBUILD_BUILTIN) += vmlinux.lds

# The table fills every slot with sys_ni_syscall, then the real calls.
CFLAGS_syscall_table.o += $(call cc-disable-warning, override-init)

obj-y += head.o setup.o process.o switch.o irq-entry.o irq.o syscall_table.o signal.o time.o traps.o ptrace.o cpuinfo.o iram.o iram_bench.o iram_bench_loops.o
