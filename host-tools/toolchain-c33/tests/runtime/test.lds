/* Link script for testsuite executables.  One flat image in SDRAM. */

OUTPUT_FORMAT("elf32-c33")
OUTPUT_ARCH(c33)
ENTRY(_start)

MEMORY
{
    sdram (rwx) : ORIGIN = 0x10000000, LENGTH = 32M
}

SECTIONS
{
    /* %r15 is the default data-area pointer; the compiler assumes it holds
       this at entry, and crt0 loads it.  */
    __dp = 0x10000000;

    /* Use a board-private section: GCC also uses .text.exit for arbitrary
       cold functions, so that generic name cannot identify our sentinel.  */
    .text : {
        *(.text.c33_test_exit)
        *(.text.start)
        *(.text .text.*)
    } > sdram

    .rodata : { *(.rodata .rodata.*) } > sdram
    .data   : { *(.data .data.*) } > sdram
    .init_array : {
        __init_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        __init_array_end = .;
    } > sdram
    .fini_array : {
        __fini_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
        KEEP(*(.fini_array))
        __fini_array_end = .;
    } > sdram
    .bss    : {
        __bss_start = . ;
        *(.bss .bss.* COMMON)
        __bss_end = . ;
    } > sdram

    /* Some upstream tests have a 14M BSS.  Put the heap after the linked
       image instead of at a fixed address that such tests can overwrite,
       and reserve the top 8M of the emulator's 32M SDRAM for the downward-
       growing stack.  grifo rounds both heap bounds to 256-byte pages.  */
    __heap_start = ALIGN(__bss_end, 256);
    __heap_end   = ORIGIN(sdram) + LENGTH(sdram) - 8M;
    __stack_top  = ORIGIN(sdram) + LENGTH(sdram) - 16;

    ASSERT(__heap_start <= __heap_end, "test image leaves no heap")
}
