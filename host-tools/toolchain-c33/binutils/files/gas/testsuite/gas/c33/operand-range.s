	.text
	add %sp,1024
	add %r0,64
	cmp %r0,32
	btst [%r0],8
