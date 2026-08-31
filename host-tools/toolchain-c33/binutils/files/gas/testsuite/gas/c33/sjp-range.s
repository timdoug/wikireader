	.text
	# sjp's base instruction is two bytes after the pseudo-instruction starts,
	# so this asks for a displacement of +0x200000: one word beyond sign22.
too_far = . + 0x200002
	sjp too_far
