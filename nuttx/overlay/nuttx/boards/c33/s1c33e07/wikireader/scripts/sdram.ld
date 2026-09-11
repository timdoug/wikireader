/* SPDX-License-Identifier: Apache-2.0 */
OUTPUT_FORMAT("elf32-c33")
OUTPUT_ARCH(c33)
ENTRY(_start)
MEMORY
{
  sdram (rwx) : ORIGIN = __ram_start, LENGTH = __ram_size
}
SECTIONS
{
  __dp = ORIGIN(sdram);
  .text :
  {
    KEEP(*(.text.start))
    *(.text .text.*)
    *(.rodata .rodata.*)
  } > sdram
  .vectors ALIGN(1024) : { KEEP(*(.vectors)) } > sdram
  .data : { *(.data .data.*) } > sdram
  .bss (NOLOAD) :
  {
    . = ALIGN(16);
    _sbss = .;
    *(.bss .bss.* COMMON)
    . = ALIGN(16);
    _ebss = .;
  } > sdram
  .idle_stack (NOLOAD) :
  {
    _sidle = .;
    . += __idle_stack_size;
    . = ALIGN(16);
    _eidle = .;
  } > sdram
  _sheap = .;
  _eheap = ORIGIN(sdram) + LENGTH(sdram);
  .debug_info 0 : { *(.debug_info .debug_info.*) }
  .debug_abbrev 0 : { *(.debug_abbrev) }
  .debug_line 0 : { *(.debug_line .debug_line.*) }
  .debug_str 0 : { *(.debug_str) }
  .debug_frame 0 : { *(.debug_frame) }
  .debug_loc 0 : { *(.debug_loc) }
  .debug_ranges 0 : { *(.debug_ranges) }
}
