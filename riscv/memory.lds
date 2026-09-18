/* The interpreter's dispatch table lives in the LCD controller's window
   buffer, which is internal RAM the visible framebuffer does not use. A0
   RAM is only 5 KB and the interpreter's code wants all of it; the window
   is another 5.5 KB at the same zero-wait speed. Grifo's code overlays use
   the same base, so they are forbidden while this reservation is active,
   and the application disables the window before running. */
SECTIONS {
    .ivram_data 0x81a00 (NOLOAD) : {
        *(.ivram_data)
    }
    /* The interpreter's dispatch table, the translator's runtime and the
       guest's divide (rv32_div.s).  The kernel's ELF loader places a
       section by its address, so an address in the window buffer is all it
       takes to run them from internal RAM. */
    .ivram_code ALIGN(4) : {
        *(.ivram_code)
    }
}
INSERT AFTER .lcd;
ASSERT(SIZEOF(.ivram_data) + SIZEOF(.ivram_code) <= 0x1600,
       "the interpreter's window-buffer reservation exceeds the LCD window")
ASSERT(SIZEOF(ovlhtml) + SIZEOF(ovlwrap) + SIZEOF(ovlhuf) + SIZEOF(ovlwebp) + SIZEOF(ovldither) == 0,
       "the dispatch table conflicts with Grifo LCD code overlays")
