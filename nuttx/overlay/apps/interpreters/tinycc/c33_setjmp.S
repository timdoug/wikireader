/* SPDX-License-Identifier: LGPL-2.0-or-later */
/* Save the C33 ABI's preserved registers and the call continuation. */
        .text
        .global tcc_c33_setjmp
        .type tcc_c33_setjmp,@function
tcc_c33_setjmp:
        xld.w [%r6+0],%r0
        xld.w [%r6+4],%r1
        xld.w [%r6+8],%r2
        xld.w [%r6+12],%r3
        ld.w %r4,%sp
        xadd %r4,4
        xld.w [%r6+16],%r4
        xld.w %r4,[%sp+0]
        xld.w [%r6+20],%r4
        ld.w %r4,0
        ret
        .size tcc_c33_setjmp,.-tcc_c33_setjmp
        .global tcc_c33_longjmp
        .type tcc_c33_longjmp,@function
tcc_c33_longjmp:
        ld.w %r4,%r7
        cmp %r4,0
        jrne 1f
        ld.w %r4,1
1:
        xld.w %r0,[%r6+0]
        xld.w %r1,[%r6+4]
        xld.w %r2,[%r6+8]
        xld.w %r3,[%r6+12]
        xld.w %r7,[%r6+16]
        ld.w %sp,%r7
        xld.w %r7,[%r6+20]
        jp %r7
        .size tcc_c33_longjmp,.-tcc_c33_longjmp
