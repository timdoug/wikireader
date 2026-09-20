ENTRY(_start)

SECTIONS
{
	. = 0;
	.text : {
		*(.text.start)
		*(.text .text.*)
		*(.rodata .rodata.*)
	}
	/DISCARD/ : {
		*(.comment)
		*(.note*)
		*(.eh_frame*)
	}
}
