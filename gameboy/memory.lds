/* hot.s's dispatch table, state block and busiest handlers live in the LCD
   controller's window buffer: internal RAM the visible framebuffer does not
   use, zero-wait like A0 RAM.  The kernel's ELF loader places a section by
   its address.  Grifo's code overlays use the same base, so they are
   forbidden, and gameboy.c disables the window before running. */
SECTIONS {
    .ivram_code 0x81a00 : {
        *(.ivram_code)
    }
}
INSERT AFTER .lcd;
ASSERT(SIZEOF(.ivram_code) <= 0x1600,
       "hot.s's window-buffer reservation exceeds the LCD window")
ASSERT(SIZEOF(ovlhtml) + SIZEOF(ovlwrap) + SIZEOF(ovlhuf) + SIZEOF(ovlwebp) + SIZEOF(ovldither) == 0,
       "hot.s's table conflicts with Grifo LCD code overlays")

/* gb.c's machine state (struct gb_s, without work RAM and VRAM) in the
   kernel's default framebuffer, 6.6 KB of zero-wait IVRAM that nothing
   shows once gameboy.c has drawn its controls and moved the panel to its
   own buffers.  gameboy.c draws first and initialises the machine after,
   and puts nothing on the default framebuffer again until the emulator is
   done with it. */
gbw_machine = __START_FrameBuffer;

/* The rest of that framebuffer holds code: hot.s's handlers that the window
   buffer and A0 RAM have no room for.  The kernel's loader clears the
   framebuffer (.lcd) and then loads this over it, in section order; after
   that nothing may draw on the default framebuffer while the emulator
   runs, so gameboy.c draws its controls into its own buffers. */
SECTIONS {
    .fbcode 0x80400 : {
        *(.fbcode)
    }
}
INSERT AFTER .lcd;
ASSERT(ADDR(.fbcode) + SIZEOF(.fbcode) <= 0x81a00,
       "hot.s's framebuffer code runs past the default framebuffer")
