# Call frame information: the CIE's entry state (CFA %sp+4, return
# address column 22 at CFA-4), register names with and without '%', the
# return address moved to a register and back, and an advance past an
# extended branch and 512 bytes, too far for DW_CFA_advance_loc1.
	.text
	.global f
	.type	f,@function
f:
	.cfi_startproc
	pushn	%r1
	.cfi_adjust_cfa_offset 8
	.cfi_offset %r1, -8
	.cfi_offset r0, -12
	sub	%sp,2
	.cfi_adjust_cfa_offset 8
	xjreq	1f
	.skip	512
1:	add	%sp,2
	.cfi_adjust_cfa_offset -8
	popn	%r1
	.cfi_adjust_cfa_offset -8
	.cfi_restore %r0
	.cfi_restore 1
	ret
	.cfi_endproc
	.size	f,.-f

	.global g
	.type	g,@function
g:
	.cfi_startproc
	pop	%r4
	.cfi_adjust_cfa_offset -4
	.cfi_register 22, %r4
	push	%r4
	.cfi_adjust_cfa_offset 4
	.cfi_offset 22, -4
	ld.w	%r12,%sp
	.cfi_def_cfa_register %r12
	.cfi_rel_offset %alr, 0
	.cfi_rel_offset %ahr, 4
	ret
	.cfi_endproc
	.size	g,.-g
