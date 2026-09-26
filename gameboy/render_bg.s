; render_bg.s - a scanline's background, for render.h, in C33 assembly.
;
;   void gbw_render_bg(struct bg_args *a);
;
; Fetches the 21 tile rows a line shows from a map row, funnel-shifts them
; into five big-endian words a bit plane, puts them through the background
; palette and either leaves the shade planes for the sprites (a->output 0)
; or dithers them straight into the framebuffer.  render.h's C does the
; same for lines with the window, and is what the host builds check; this is
; the common case, in passes small enough to keep their values in registers,
; which the compiler's single loop could not.
;
; Every pass is unrolled.  In A0 RAM a taken branch still costs about five
; cycles and a load two, where most instructions take one: the loops'
; back edges and a per-word branch on the dither mode were a fifth of the
; routine.  The two framebuffer stores of a word are split by the next
; word's loads, since a store straight after a store waits for the first.
;
; struct bg_args, all words:
	.set	MAP, 0			; the map row, 32 bytes
	.set	TILES, 4		; the tile block plus the tile row's offset
	.set	FIRST, 8		; the first column shown
	.set	FLIP, 12		; 0x80 for signed tile numbers, else 0
	.set	SHIFT, 16		; SCX & 7
	.set	IDENTITY, 20		; BGP is 0xe4: shades are the colours
	.set	STREAM0, 24		; 6 words a plane, for the tile rows
	.set	STREAM1, 28
	.set	PLANE0, 32		; 5 words a plane, the shades
	.set	PLANE1, 36
	.set	OUT, 40			; the framebuffer row, halfword aligned
	.set	HALF, 44		; dither masks for this row
	.set	QUARTER, 48
	.set	DITHERED, 52
	.set	OUTPUT, 56		; dither into OUT; else leave the planes
	.set	MASKS, 60		; c0 d10 c2 d32 for shade bit 0, then bit 1
	.set	ROW, 92			; 64 bytes of A0 RAM for the map row, twice

	.section .fastcode,"ax"
	.align	1
	.globl	gbw_render_bg

; One tile: the map byte at %r0, its row's two planes from %r2, shifted into
; %r4 (plane 0) and %r5 (plane 1).
	.macro	TILE
	ld.ub	%r9,[%r0]+
	xor	%r9,%r3
	sll	%r9,4
	add	%r9,%r2
	ld.uh	%r10,[%r9]		; plane 0 low byte, plane 1 high
	sll	%r4,8
	ld.ub	%r11,%r10
	or	%r4,%r11
	srl	%r10,8
	sll	%r5,8
	or	%r5,%r10
	.endm

gbw_render_bg:
	pushn	%r3
	ld.w	%r13,%r6		; the arguments, for the whole call

; ---- the map row into A0 RAM twice over: eight word loads from SDRAM,
; where 21 byte loads each waited on the memory, and 21 columns from any
; start then read straight on --------------------------------------------
	xld.w	%r0,[%r13+MAP]
	xld.w	%r1,[%r13+ROW]
	.rept	8
	ld.w	%r9,[%r0]+
	xld.w	[%r1+32],%r9
	ld.w	[%r1]+,%r9
	.endr

; ---- fetch: 21 tiles into the streams, four to a word ------------------
	xld.w	%r0,[%r13+ROW]
	xld.w	%r9,[%r13+FIRST]
	add	%r0,%r9
	xld.w	%r2,[%r13+TILES]
	xld.w	%r3,[%r13+FLIP]
	xld.w	%r7,[%r13+STREAM0]
	xld.w	%r8,[%r13+STREAM1]
	ld.w	%r4,0
	ld.w	%r5,0
	.rept	5
	TILE
	TILE
	TILE
	TILE
	ld.w	[%r7]+,%r4
	ld.w	[%r8]+,%r5
	.endr
	TILE				; the 21st, which a scroll of up to 7
	sll	%r4,24			; pixels reaches into
	sll	%r5,24
	ld.w	[%r7],%r4
	ld.w	[%r8],%r5

; ---- funnel: the 160 pixels from SHIFT bits in, to the planes ----------
	xld.w	%r7,[%r13+STREAM0]
	xld.w	%r8,[%r13+STREAM1]
	xld.w	%r0,[%r13+PLANE0]
	xld.w	%r1,[%r13+PLANE1]
	xld.w	%r2,[%r13+SHIFT]
	ld.w	%r3,31
	sub	%r3,%r2			; the second half shifts in two steps,
	ld.w	%r4,[%r7]+		; so that a shift of 0 is not one of 32
	ld.w	%r5,[%r8]+
	.rept	5
	ld.w	%r9,[%r7]+
	ld.w	%r10,[%r8]+
	sll	%r4,%r2
	ld.w	%r11,%r9
	srl	%r11,1
	srl	%r11,%r3
	or	%r4,%r11
	ld.w	[%r0]+,%r4
	sll	%r5,%r2
	ld.w	%r11,%r10
	srl	%r11,1
	srl	%r11,%r3
	or	%r5,%r11
	ld.w	[%r1]+,%r5
	ld.w	%r4,%r9
	ld.w	%r5,%r10
	.endr

; ---- palette: both shade planes at once, all eight masks in registers --
; low = c0 ^ (d10 & lo), high = c2 ^ (d32 & lo), shade = low ^ ((high ^
; low) & hi).  The argument pointer waits in %alr, free of multiplies here.
	xld.w	%r9,[%r13+IDENTITY]
	cmp	%r9,0
	xjrne	.Lshaded
	xld.w	%r7,[%r13+PLANE0]	; colour bit 0, then shade bit 0
	xld.w	%r8,[%r13+PLANE1]	; colour bit 1, then shade bit 1
	xld.w	%r0,[%r13+MASKS+16]	; shade bit 1's masks
	xld.w	%r1,[%r13+MASKS+20]
	xld.w	%r2,[%r13+MASKS+24]
	xld.w	%r3,[%r13+MASKS+28]
	xld.w	%r12,[%r13+MASKS+0]	; shade bit 0's
	xld.w	%r14,[%r13+MASKS+4]
	xld.w	%r6,[%r13+MASKS+8]
	ld.w	%alr,%r13
	xld.w	%r13,[%r13+MASKS+12]
	.rept	5
	ld.w	%r4,[%r7]		; lo
	ld.w	%r5,[%r8]		; hi
	ld.w	%r9,%r1			; shade bit 1
	and	%r9,%r4
	xor	%r9,%r0			; low
	ld.w	%r10,%r3
	and	%r10,%r4
	xor	%r10,%r2		; high
	xor	%r10,%r9
	and	%r10,%r5
	xor	%r10,%r9
	ld.w	[%r8]+,%r10
	ld.w	%r9,%r14		; shade bit 0
	and	%r9,%r4
	xor	%r9,%r12		; low
	ld.w	%r10,%r13
	and	%r10,%r4
	xor	%r10,%r6		; high
	xor	%r10,%r9
	and	%r10,%r5
	xor	%r10,%r9
	ld.w	[%r7]+,%r10
	.endr
	ld.w	%r13,%alr
.Lshaded:

; ---- dither into the framebuffer, unless sprites go on first -----------
	xld.w	%r9,[%r13+OUTPUT]
	cmp	%r9,0
	xjreq	.Ldone
	jp	.Ldither_planes

; The dither alone, after C has put the sprites on the shade planes:
;   void gbw_render_dither(struct bg_args *a);
	.globl	gbw_render_dither
gbw_render_dither:
	pushn	%r3
	ld.w	%r13,%r6
.Ldither_planes:
	xld.w	%r7,[%r13+PLANE0]
	xld.w	%r8,[%r13+PLANE1]
	xld.w	%r0,[%r13+OUT]
	xld.w	%r1,[%r13+HALF]
	xld.w	%r2,[%r13+QUARTER]
	xld.w	%r3,[%r13+DITHERED]
	ld.w	%r4,[%r7]+		; shade bits 0 and 1 of the first word
	ld.w	%r5,[%r8]+
	cmp	%r3,0
	xjreq	.Lthreshold

; Black for shade 3, half the pixels for 2, a quarter for 1: the word in
; %r9, its second halfword stored after the next word is loaded.
	.rept	5
	ld.w	%r9,%r4
	or	%r9,%r1
	and	%r9,%r5
	not	%r10,%r5
	and	%r10,%r4
	and	%r10,%r2
	or	%r9,%r10
	swap	%r9,%r9			; the leftmost byte first in memory
	ld.h	[%r0]+,%r9
	ld.w	%r4,[%r7]+		; the next word (one past the planes'
	ld.w	%r5,[%r8]+		; end, the last time: harmless reads)
	srl	%r9,16
	ld.h	[%r0]+,%r9
	.endr
	popn	%r3
	ret

.Lthreshold:				; black from shade 2
	.rept	5
	swap	%r9,%r5
	ld.h	[%r0]+,%r9
	ld.w	%r5,[%r8]+
	srl	%r9,16
	ld.h	[%r0]+,%r9
	.endr
.Ldone:
	popn	%r3
	ret
