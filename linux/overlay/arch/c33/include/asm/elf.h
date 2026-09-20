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
#define elf_check_arch(x) (1)
#define elf_check_fdpic(x) (0)
#define ELF_CORE_COPY_REGS(dest, regs) \
	do { memcpy((dest), (regs), sizeof(*(regs))); } while (0);
#define SET_PERSONALITY(ex) set_personality(PER_LINUX)
#endif
