/* Doom borrows the disabled LCD window buffer for one 64x64 flat texture.
   The visible framebuffer ends here. Grifo's overlays use the same base,
   so forbid those sections while this reservation is active. */
SECTIONS {
    .texture_cache 0x81a00 (NOLOAD) : {
        *(.texture_cache)
    }
}
INSERT AFTER .lcd;
ASSERT(SIZEOF(.texture_cache) <= 0x1600, "Doom texture cache exceeds LCD window RAM")
ASSERT(SIZEOF(ovlhtml) + SIZEOF(ovlwrap) + SIZEOF(ovlhuf) + SIZEOF(ovlwebp) + SIZEOF(ovldither) == 0,
       "Doom texture cache conflicts with Grifo LCD code overlays")
