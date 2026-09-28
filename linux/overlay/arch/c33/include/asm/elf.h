/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_C33_ELF_H
#define _ASM_C33_ELF_H
#include <uapi/asm/elf.h>
#include <asm/ptrace.h>
#define ELF_CLASS ELFCLASS32
#define ELF_DATA ELFDATA2LSB
#define ELF_ARCH 107 /* EM_SE_C33 */
#define ELF_EXEC_PAGESIZE PAGE_SIZE
#define ELF_ET_DYN_BASE 0
#define ELF_HWCAP 0
#define ELF_PLATFORM NULL
#define elf_check_arch(x) ((x)->e_machine == ELF_ARCH)

/*
 * FDPIC programs, whose segments go anywhere, say so in e_flags.  The kernel
 * hands one the load maps of itself and of its interpreter, and the dynamic
 * section, in the first three argument registers; the program works out its
 * own %r15 from them.
 */
#define EF_C33_FDPIC 0x00000001
#define elf_check_fdpic(x) ((x)->e_flags & EF_C33_FDPIC)
#define ELF_FDPIC_CORE_EFLAGS EF_C33_FDPIC
#define ELF_FDPIC_PLAT_INIT(_regs, _exec_map_addr, _interp_map_addr,	\
			    _dynamic_addr)				\
	do {								\
		(_regs)->r[6] = (_exec_map_addr);			\
		(_regs)->r[7] = (_interp_map_addr);			\
		(_regs)->r[8] = (_dynamic_addr);			\
	} while (0)
#define ELF_CORE_COPY_REGS(dest, regs) \
	do { memcpy((dest), (regs), sizeof(*(regs))); } while (0);
#define SET_PERSONALITY(ex) set_personality(PER_LINUX)
#endif
