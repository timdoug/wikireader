/* Code overlays in the chip's zero-wait IVRAM.
 *
 * The C33 has no instruction cache and code fetched from SDRAM spends a
 * third of its cycles waiting.  The A0 RAM area that application.lds
 * gives applications holds the Zstandard sequence loop permanently; the
 * next hottest functions run one phase at a time, so they share the LCD
 * controller's 5632-byte window buffer in IVRAM instead.  Each is linked
 * to run there (application.lds, OVERLAY) and stored in SDRAM, and the
 * phase that needs it copies it in first.  The kernel draws into that
 * buffer only for the search screen's highlight, and the reader's scroll
 * overlay uses a buffer of its own, so an article load never shares it.
 *
 * On the host the functions are simply called in place. */
#ifndef WIKIREADER_ZIM_OVERLAY_H
#define WIKIREADER_ZIM_OVERLAY_H

#if defined(__c33__)

/* The window buffer follows the 6656-byte frame buffer at the start of
 * IVRAM (samo-lib/grifo/lds/grifo.lds). */
#define ZIM_OVERLAY_BASE ((unsigned char *)0x00081a00)
#define ZIM_OVERLAY_SIZE 5632

#define ZIM_OVERLAY_SECTION(name) __attribute__((section(name)))

/* Scratch in the A0 RAM area left after the sequence loop (.fastbss,
 * application.lds): a cycle an access and no SDRAM row.  Shared by phases
 * that never run at the same time: the FSE table builder (632 bytes of
 * per-symbol tables) and the wrapper (384 bytes of width and word-break
 * tables), and WebP's boolean-coder log table (256 bytes).  Nothing in the
 * kernel touches this RAM. */
#define ZIM_FAST_SCRATCH_SIZE 640
extern unsigned char zim_fast_scratch[ZIM_FAST_SCRATCH_SIZE];

/* Copy an overlay in unless it is already there. */
void zim_overlay_ensure(const void *load_start, const void *load_stop);
/* Forget what the buffer holds, e.g. after the search screen may have
 * drawn into it. */
void zim_overlay_invalidate(void);

#define ZIM_OVERLAY_ENSURE(name) \
	zim_overlay_ensure(__load_start_##name, __load_stop_##name)

extern const unsigned char __load_start_ovlhtml[], __load_stop_ovlhtml[];
extern const unsigned char __load_start_ovlwrap[], __load_stop_ovlwrap[];
extern const unsigned char __load_start_ovlhuf[], __load_stop_ovlhuf[];
extern const unsigned char __load_start_ovlwebp[], __load_stop_ovlwebp[];
extern const unsigned char __load_start_ovldither[], __load_stop_ovldither[];

#else

#define ZIM_OVERLAY_SECTION(name)
#define ZIM_OVERLAY_ENSURE(name) ((void)0)
#define zim_overlay_invalidate() ((void)0)

#endif

#endif
