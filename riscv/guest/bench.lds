OUTPUT_ARCH(riscv)
ENTRY(_start)

MEMORY
{
	ram (rwx) : ORIGIN = 0x80000000, LENGTH = 4M
}

SECTIONS
{
	.text : {
		*(.text.start)
		*(.text .text.*)
		*(.rodata .rodata.*)
		. = ALIGN(4);
	} > ram

	.data : {
		__global_pointer$ = . + 0x800;
		*(.sdata .sdata.*)
		*(.data .data.*)
		. = ALIGN(4);
	} > ram

	.bss (NOLOAD) : {
		__bss_start = . ;
		*(.sbss .sbss.*)
		*(.bss .bss.*)
		*(COMMON)
		. = ALIGN(4);
		__bss_end = . ;
	} > ram

	/* Working set plus stack.  Kept well inside 4 MB so the same image
	   runs on a 16 MB board's guest as on a 32 MB one. */
	.heap (NOLOAD) : {
		. = ALIGN(16);
		__heap_start = . ;
		. = . + 1M ;
		__heap_end = . ;
		. = . + 64K ;
		__stack_top = . ;
	} > ram
}
