/* Bare-metal link for differential test programs: SDRAM at 0x10000000.
 *
 * __dp must sit at or below every symbol it addresses -- the linker rejects
 * "Default Data area pointer value is larger than symbol address value" --
 * so it is pinned to the start of the image rather than to .data. */
OUTPUT_FORMAT("elf32-c33")
OUTPUT_ARCH(c33)
ENTRY(_start)

MEMORY {
	sdram (rwx) : ORIGIN = 0x10000000, LENGTH = 32M
}

SECTIONS {
	__dp = 0x10000000;
	.text : { *(.text) *(.text.*) *(.rodata) *(.rodata.*) } > sdram
	.data : { *(.data) *(.data.*) } > sdram
	.bss  : { __bss_start = . ; *(.bss) *(COMMON) ; __bss_end = . ; } > sdram
	__stack_top = 0x11000000;
}
