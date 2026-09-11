OUTPUT_FORMAT("elf32-c33")
OUTPUT_ARCH(c33)
ENTRY(_start)
MEMORY {
    a0 (rwx) : ORIGIN = 0xc00, LENGTH = 0x13c0
    sdram (rwx) : ORIGIN = 0x10000000, LENGTH = 32M
}
SECTIONS {
    __dp = 0x10000000;
    .fastcode : { *(.fastcode) } > a0
    .text : { *(.text) *(.text.*) *(.rodata) *(.rodata.*) } > sdram
    .data : { *(.data) *(.data.*) } > sdram
    .bss : { __bss_start = .; *(.bss) *(COMMON); __bss_end = .; } > sdram
    __stack_top = 0x11000000;
}
