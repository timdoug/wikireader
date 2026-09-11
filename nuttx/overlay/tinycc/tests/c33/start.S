/* SPDX-License-Identifier: LGPL-2.0-or-later */
.section .text.start,"ax"
.global _start
_start:
    xld.w %r4,0x11000000
    ld.w %sp,%r4
    xld.w %r15,0x10000000
    xcall main
.global test_done
test_done:
    jp test_done
