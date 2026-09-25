ENTRY(_start)

SECTIONS
{
	/* binfmt_flat reserves encoded zero as the null pointer. */
	. = 4;
	.text : {
		*(.text.start)
		*(.text .text.*)
		KEEP(*(.init))
		KEEP(*(.fini))
		*(.rodata .rodata.*)
		. = ALIGN(4);
	}
	.data : {
		/* -msep-data code reaches .data and .bss as %r15 + (sym - __dp);
		   binfmt_flat loads %r15 with this segment's address. */
		__dp = .;
		*(.data .data.*)
		*(.sdata .sdata.*)
		. = ALIGN(4);
		PROVIDE_HIDDEN(__preinit_array_start = .);
		KEEP(*(.preinit_array))
		PROVIDE_HIDDEN(__preinit_array_end = .);
		PROVIDE_HIDDEN(__init_array_start = .);
		KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
		KEEP(*(.init_array .ctors))
		PROVIDE_HIDDEN(__init_array_end = .);
		PROVIDE_HIDDEN(__fini_array_start = .);
		KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*) SORT_BY_INIT_PRIORITY(.dtors.*)))
		KEEP(*(.fini_array .dtors))
		PROVIDE_HIDDEN(__fini_array_end = .);
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
