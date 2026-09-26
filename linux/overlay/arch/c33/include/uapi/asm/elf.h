/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_ASM_C33_ELF_H
#define _UAPI_ASM_C33_ELF_H
#include <linux/types.h>
typedef unsigned long elf_greg_t;
/* One per word of struct pt_regs, which ELF_CORE_COPY_REGS copies whole. */
#define ELF_NGREG 24
typedef elf_greg_t elf_gregset_t[ELF_NGREG];
typedef struct { unsigned long unused; } elf_fpregset_t;
#endif
