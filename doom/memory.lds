/* Doom borrows the disabled LCD window buffer for one 64x64 flat texture.
   The visible framebuffer ends here. Grifo's overlays use the same base,
   so forbid those sections while this reservation is active. */
SECTIONS {
    .texture_cache 0x81a00 (NOLOAD) : {
        *(.texture_cache)
    }
}
INSERT AFTER .lcd;

/* libgcc's divide runs from A0 RAM here, where the fetch is free: Doom calls
   it 447 times a frame and in SDRAM three quarters of its cost is fetching
   its own code -- the entry line, the return, and a loop that will not stay
   in the fetch window.  Only the division itself moves.  Its callers reach
   it because the compiler calls __udivsi3/__umodsi3/__divsi3 in .text and
   those are long jumps into this, which is what the A0 bridge Doom used to
   carry did by hand.

   The link warns twice that `fastram' is undeclared and then redeclared:
   this script is given to ld before application.lds, which declares the
   region. The placement is still the region's -- an oversized .fastdiv
   fails the link rather than running past A0 into the suspend scratch. */
SECTIONS {
    .fastdiv : {
        *libgcc.a:udivmodsi4.o(.text)
    } > fastram
}
INSERT AFTER .fastcode;
ASSERT(SIZEOF(.texture_cache) <= 0x1600, "Doom texture cache exceeds LCD window RAM")
ASSERT(SIZEOF(ovlhtml) + SIZEOF(ovlwrap) + SIZEOF(ovlhuf) + SIZEOF(ovlwebp) + SIZEOF(ovldither) == 0,
       "Doom texture cache conflicts with Grifo LCD code overlays")
