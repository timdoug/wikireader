/* SPDX-License-Identifier: GPL-2.0 */
	.section .text.start,"ax"
	.global _start
	.type _start,@function
_start:
	/* CALL leaves a position-independent pointer to the message table. */
	call .Lmessages_end
.Lmessages:
.Lbanner_size:
	.byte .Lbanner_end-.Lbanner
.Lbanner:
	.ascii "WikiReader native C33 userspace is alive!\n"
.Lbanner_end:
	.byte .Lhelp_end-.Lhelp
.Lhelp:
	.ascii "c33 shell: h=help p=getpid\n"
.Lhelp_end:
	.byte .Lprompt_end-.Lprompt
.Lprompt:
	.ascii "c33> "
.Lprompt_end:
	.byte .Lrx_end-.Lrx
.Lrx:
	.ascii "UART RX reached Linux userspace.\n"
.Lrx_end:
	.byte .Lpid_end-.Lpid
.Lpid:
	.ascii "pid "
.Lpid_end:
	.byte .Lnewline_end-.Lnewline
.Lnewline:
	.ascii "\n"
.Lnewline_end:
.Lmessages_end:
	pop %r6
	call init_main
.Lhang:
	jp .Lhang
	.size _start,.-_start

	.text
	.global c33_fcntl
	.type c33_fcntl,@function
c33_fcntl:
	xld.w %r4,25
	int 0
	ret
	.size c33_fcntl,.-c33_fcntl

	.global c33_read
	.type c33_read,@function
c33_read:
	xld.w %r4,63
	int 0
	ret
	.size c33_read,.-c33_read

	.global c33_write
	.type c33_write,@function
c33_write:
	xld.w %r4,64
	int 0
	ret
	.size c33_write,.-c33_write

	.global c33_getpid
	.type c33_getpid,@function
c33_getpid:
	xld.w %r4,172
	int 0
	ret
	.size c33_getpid,.-c33_getpid
