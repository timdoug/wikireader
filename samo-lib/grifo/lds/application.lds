/* application.lds - linker script */

/*
 * Notes:
 *
 * 1. LENGTH() does not work so have to use hard coded numbers
 * 2. ORIGIN() also does not work
 * 3. __START_x, __END_x are byte aligned addresses
 * 4. __SIZE_x is the number of bytes in the block
 */

OUTPUT_FORMAT("elf32-c33")

OUTPUT_ARCH(c33)

/* grifo jumps to e_entry, and an application's entry is grifo_main.  Naming
   it is not optional: without ENTRY the entry address defaults to the start
   of .text, which is only grifo_main if the compiler happened to emit it
   first.  gcc 3.3.2 always did.  gcc 16 reorders functions, so init.app --
   whose object holds five of them -- started in the middle of
   CursorPosition and returned -1 before it could print anything.  wiki.o
   holds one function, which is why wiki.app was unaffected and this stayed
   hidden.  */
ENTRY(grifo_main)

MEMORY
{
    /* sdram must be first or linker gives errors */
    sdram   (rwx): ORIGIN = 0x10000000 + 256k, LENGTH = 32M - 256k
    ivram   (rwx): ORIGIN = 0x00080000,        LENGTH = 12k
}

SECTIONS
{
    .lcd (NOLOAD) : {
        __START_FrameBuffer = . ;
        . = . + 12k /* LENGTH(ivram) */ ;
        __END_FrameBuffer = . ;
    } > ivram

    /* the compiler/linker assumes r15 is preset to this value at the start */
    __dp = 0x10000000 + 256k /* ORIGIN(sdram) */ ;

    .text : {
         __START_text = . ;
         /* .text.* too; see the note in grifo.lds.  */
         *(.text .text.*)
    } > sdram
    __END_text = . ;

    . = ALIGN(1024);

    .rodata : {
         __START_rodata = . ;
         *(.rodata)
    } > sdram
    __END_rodata = . ;

    /* .data before .bss.  With .bss first, the LOAD segment's filesz has to
       stretch past it to reach .data, so every byte of .bss ends up stored
       in the file as zeroes -- 27kB of the kernel, read off the card at
       every boot.  binutils 2.10.1 split this into two segments; modern ld
       does not.  Putting .bss last lets it stay NOBITS.

       The wildcard patterns matter for the same reason .text.* did:
       gcc 4 and later split sections, and an unmatched one becomes an
       orphan placed wherever ld likes.  */

    .data : {
         __START_data = . ;
         *(.data .data.*)
    } > sdram
    __END_data = . ;

    .bss : {
         __START_bss = . ;
         *(.bss .bss.* COMMON)
    } > sdram
     __END_bss = . ;

    . = ALIGN(1024);

    __END_program = . ;

    __START_text = ADDR(.text);
    __END_text = __START_text + SIZEOF(.text);

    __START_rodata = ADDR(.rodata);
    __END_rodata = __START_rodata + SIZEOF(.rodata);

    __START_data = ADDR(.data);
    __END_data = __START_data + SIZEOF(.data);

    __START_bss = ADDR(.bss);
    __END_bss = __START_bss + SIZEOF(.bss);

}
