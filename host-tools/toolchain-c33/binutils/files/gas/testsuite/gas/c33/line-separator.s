	.text
	.global separator_a ` separator_a: ` swaph %r0,%r1
	.section ".separator_probe","a" ` .long separator_a ` .previous
	.global separator_b ` separator_b: ` adc %r0,%r1
