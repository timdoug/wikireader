/* SPDX-License-Identifier: LGPL-2.0-or-later */
OUTPUT_FORMAT("elf32-c33")
OUTPUT_ARCH(c33)
ENTRY(_start)
SECTIONS {
    . = 0x10000000;
    __dp = .;
    .text : { *(.text.start) *(.text*) *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(.bss*) *(COMMON) }
}
