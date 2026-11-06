/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_C33_TRAPS_H
#define _ASM_C33_TRAPS_H

struct pt_regs;

const char *c33_vector_name(unsigned int vector);
void c33_show_regs(const struct pt_regs *regs, const char *loglvl);
void __noreturn c33_die(unsigned int vector, struct pt_regs *regs);

#endif
