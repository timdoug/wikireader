/* grifo.lds - linker script */

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

/* main is the kernel's entry point: it is what sets up %sp, so nothing may
   run before it.  Every loader that starts this image jumps to e_entry --
   the emulator, drivers/src/elf32.c and mbr/rs232-loader.c all do -- so
   naming it here is what actually places it, rather than hoping the
   compiler emits main first.  */
ENTRY(main)

MEMORY
{
    /* sdram must be first or linker gives errors */
    ksdram  (rwx): ORIGIN = 0x10000000,        LENGTH = 256k
    sdram   (rwx): ORIGIN = 0x10000000 + 256k, LENGTH = 32M - 256k
    a0ram   (rwx): ORIGIN = 0x00000000,        LENGTH = 8k
    ivram   (rwx): ORIGIN = 0x00080000,        LENGTH = 12k
    dstram  (rwx): ORIGIN = 0x00084000,        LENGTH = 2k
}

SECTIONS
{
    .internal (NOLOAD) : {
         __START_NullPointer = . ;
         . = . + 1k ;       /* 1 kibi bytes in case of write to null pointer */
         __END_NullPointer = . ;
         __START_VectorTable = . ;
         . = . + 1k ;       /* 256 entry vector table = 1 kibi bytes */
         __END_VectorTable = . ;
         __START_SuspendCode = . ;
         . = . + 6k - 64 ;  /* suspend can use the remainder */
         __END_SuspendCode = . ;
         __START_SuspendScratch = . ;
         . = . + 64 ;       /* registers SuspendCode saves while SDRAM is off */
         __END_SuspendScratch = . ;
    } > a0ram

    __SIZE_NullPointer = __END_NullPointer - __START_NullPointer ;

    __SIZE_VectorTable = __END_VectorTable - __START_VectorTable ;
    __ENTRIES_VectorTable =  __SIZE_VectorTable / 4 ; /* each entry is one word (4 bytes) */

    __SIZE_SuspendCode = __END_SuspendCode - __START_SuspendCode ;

    __BYTES_FrameBuffer = (256 / 8) * 208 ; /* 240 (round up to uint32_t) * 208 pixels */
    __BYTES_WindowBuffer = 12k - __BYTES_FrameBuffer ;

    .lcd (NOLOAD) : {
        __START_FrameBuffer = . ;
        . = . + __BYTES_FrameBuffer ;
        __END_FrameBuffer = . ;
        __START_WindowBuffer = . ;
        . = . + __BYTES_WindowBuffer ;
        __END_WindowBuffer = . ;
    } > ivram

    __SIZE_FrameBuffer = __END_FrameBuffer - __START_FrameBuffer ;
    __SIZE_WindowBuffer = __END_WindowBuffer - __START_WindowBuffer ;

    .descriptor (NOLOAD) : {
        __START_descriptors = . ;
        *(.dstram)              /* DMA descriptors, see src/sd_dma.c */
        . = __START_descriptors + 2k /* LENGTH(dstram) */ ;
        __END_descriptors = . ;
    } > dstram

    /* the compiler/linker assumes r15 is preset to this value at the start */
     __dp = 0x10000000 /* ORIGIN(ksdram) */ ;

    .text : {
         __START_text = . ;
         /* .text.* as well as .text: gcc 4 and later split functions into
             .text.startup, .text.unlikely, .text.hot and so on.  Keeping
             main.o's pieces together and first is still worth doing for
             locality, but it is no longer what makes main the entry --
             ENTRY(main) above does that.  Ordering alone was not enough:
             it holds at -O2, where -freorder-functions puts main in
             .text.startup, and fails at -O1, where main stays in plain
             .text and "process" is emitted ahead of it.  */
         build/main.o(.text.startup .text.startup.*)
         build/main.o(.text .text.*)
         *(.text .text.*)
    } > ksdram
    __END_text = . ;

    .suspend : {
         __START_suspend = . ;
         *(.suspend_text)
         *(.suspend_text.*)
    } > ksdram
    __END_suspend = . ;
    __SIZE_suspend = SIZEOF(.suspend);

    . = ALIGN(1024);

    .rodata : {
         __START_rodata = . ;
         *(.rodata .rodata.*)
    } > ksdram
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
    } > ksdram
    __END_data = . ;

    .bss : {
         __START_bss = . ;
         *(.bss .bss.* COMMON)
    } > ksdram
     __END_bss = . ;

    . = ALIGN(1024);

    __END_program = . ;


    .user_area (NOLOAD) : {
        __dp_user = . /* ORIGIN(usdram) */ ;
    } > sdram


    /*__MAIN_STACK = ORIGIN(sdram) + LENGTH(sdram) - 4; */
    __MAIN_STACK = 0x10000000 + 32M - 4;
    __MAIN_STACK_LIMIT = __MAIN_STACK - 1M;


    __START_text = ADDR(.text);
    __END_text = __START_text + SIZEOF(.text);

    __START_rodata = ADDR(.rodata);
    __END_rodata = __START_rodata + SIZEOF(.rodata);

    __START_data = ADDR(.data);
    __END_data = __START_data + SIZEOF(.data);

    __START_bss = ADDR(.bss);
    __END_bss = __START_bss + SIZEOF(.bss);

    /* make sure heap is aligned to 1 kibi byte boundary */
    __START_heap = (__END_program + 1023) & (~ 1023);
    __END_heap = __MAIN_STACK_LIMIT & (~ 1023);
    __SIZE_heap = __END_heap - __START_heap;

    /* Debugging metadata has no target-memory footprint.  Give it the
       conventional zero VMA explicitly so orphan placement cannot make a
       larger modern build appear to overflow ksdram. */
    .debug_aranges  0 : { *(.debug_aranges) }
    .debug_pubnames 0 : { *(.debug_pubnames) }
    .debug_info     0 : { *(.debug_info .gnu.linkonce.wi.*) }
    .debug_abbrev   0 : { *(.debug_abbrev) }
    .debug_line     0 : { *(.debug_line .debug_line.*) }
    .debug_frame    0 : { *(.debug_frame) }
    .debug_str      0 : { *(.debug_str) }
    .debug_loc      0 : { *(.debug_loc) }
    .debug_ranges   0 : { *(.debug_ranges) }
    .comment        0 : { *(.comment) }
    .stab           0 : { *(.stab) }
    .stabstr        0 : { *(.stabstr) }
}
