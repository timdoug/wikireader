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

    /* .text.exit first, so that "exit" is always at 0x10000000 and the
       driver can breakpoint it without reading symbols per test.  */
    .text : {
        *(.text.exit)
        *(.text.start)
        *(.text .text.*)
    } > sdram

    .rodata : { *(.rodata .rodata.*) } > sdram
    .data   : { *(.data .data.*) } > sdram
    .bss    : {
        __bss_start = . ;
        *(.bss .bss.* COMMON)
        __bss_end = . ;
    } > sdram

    . = ALIGN(16);
    __heap_start = . ;

    /* Well clear of the image; tests are small and the emulator gives us
       the whole 32M.  */
    __stack_top = 0x10000000 + 8M - 16;
}
