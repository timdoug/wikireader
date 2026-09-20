/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_C33_IO_H
#define _ASM_C33_IO_H
#define IO_SPACE_LIMIT 0xffffffff
#define ioremap(addr, size) ((void __iomem *)(unsigned long)(addr))
#define iounmap(addr) do { } while (0)
#include <asm-generic/io.h>
#endif
