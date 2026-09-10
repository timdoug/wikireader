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
    /* The part of the chip's zero-wait A0 RAM the kernel leaves free: its
       own relocated code (suspend and SDRAM retiming) stays below 0xc00,
       which grifo.lds asserts, and the 64-byte suspend scratch area sits
       at the top.  Code placed here by an application runs without the
       instruction-fetch stalls SDRAM imposes; the kernel's ELF loader
       copies the section in like any other. */
    fastram (rwx): ORIGIN = 0x00000c00,        LENGTH = 0x13c0
}

SECTIONS
{
    /* The kernel's frame buffer (grifo.lds: 256/8 * 208 bytes); the rest of
       IVRAM is the LCD controller's window buffer, which the ZIM reader
       borrows for code overlays below. */
    .lcd (NOLOAD) : {
        __START_FrameBuffer = . ;
        . = . + 0x1a00 ;
        __END_FrameBuffer = . ;
    } > ivram

    /* the compiler/linker assumes r15 is preset to this value at the start */
    __dp = 0x10000000 + 256k /* ORIGIN(sdram) */ ;

    .text : {
         __START_text = . ;
         /* .text.* too; see the note in grifo.lds.  */
         *(.text .text.*)
         /* Bulk-copy dispatch is appended to preserve existing hot loops'
            instruction-queue alignment when the ZIM app adds wrappers. */
         *(.copycode)
         /* so the overlays stored after it are word-aligned and copied
            into IVRAM by words (zim_overlay.c) */
         . = ALIGN(4);
    } > sdram
    __END_text = . ;

    .fastcode : {
         __START_fastcode = . ;
         *(.fastcode)
         *(.fastcode.*)   /* .fastcode.pic: position-independent tests */
    } > fastram
    __END_fastcode = . ;
    /* Zero-initialised data beside it (the Zstandard sequence state); the
       kernel's loader clears NOBITS sections wherever they are. */
    .fastbss (NOLOAD) : {
         __START_fastbss = . ;
         *(.fastbss .fastbss.*)
    } > fastram
    __END_fastbss = . ;

    /* Code overlays (zim/zim_overlay.h): each is linked to run in the
       window buffer at 0x81a00 and stored after .text in SDRAM, from where
       the reader copies it in before the phase that needs it.  The C33's
       calls and jumps are PC-relative even in their long forms, so the code
       has to be linked for the address it runs at.  The kernel's ELF loader
       places sections by their address, so after linking the reader's
       Makefile moves each section's address to its load address (objcopy
       --change-section-vma); ld's __load_start_* symbols still give the
       load addresses. */
    OVERLAY : NOCROSSREFS {
        ovlhtml { *(ovlhtml) . = ALIGN(4); }
        ovlwrap { *(ovlwrap) . = ALIGN(4); }
        ovlhuf  { *(ovlhuf) . = ALIGN(4); }
        ovlwebp { *(ovlwebp) . = ALIGN(4); }
        ovldither { *(ovldither) . = ALIGN(4); *(ovlditherdata) }
    } > ivram AT> sdram
    ASSERT(SIZEOF(ovlhtml) <= 0x1600, "the HTML overlay exceeds the IVRAM window buffer")
    ASSERT(SIZEOF(ovlwrap) <= 0x1600, "the wrap overlay exceeds the IVRAM window buffer")
    ASSERT(SIZEOF(ovlhuf) <= 0x1600, "the Huffman overlay exceeds the IVRAM window buffer")
    ASSERT(SIZEOF(ovlwebp) <= 0x1600, "the WebP overlay exceeds the IVRAM window buffer")
    ASSERT(SIZEOF(ovldither) <= 0x1600, "the dither overlay exceeds the IVRAM window buffer")

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
