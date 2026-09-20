ENTRY(_start)

SECTIONS
{
	/* binfmt_flat deliberately leaves an encoded zero pointer untouched. */
	. = 4;
	.text : {
		*(.text.start)
		*(.text .text.*)
		*(.rodata .rodata.*)
		. = ALIGN(4);
	}
	.data : {
		*(.data .data.*)
		*(.sdata .sdata.*)
	}
	.bss : {
		*(.bss .bss.*)
		*(.sbss .sbss.*)
		*(COMMON)
	}
	/DISCARD/ : {
		*(.comment)
		*(.note*)
		*(.eh_frame*)
	}
}
