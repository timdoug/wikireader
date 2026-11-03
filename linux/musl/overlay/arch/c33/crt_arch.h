/* The kernel, or ld.so for a dynamic program, enters with the program's
 * load map in %r6, the interpreter's in %r7 (zero for a static program,
 * or for ld.so run as a program) and the interpreter's dynamic section in
 * %r8.  Nothing is relocated yet and %r15 is not set: c33_fdpic_fixup
 * relocates the words .rofixup lists and returns the last one, the
 * relocated address of __dp, which is %r15.  The text has no PC-relative
 * way to reach the list, so it is passed the list's link-time bounds,
 * which it translates through the load map. */

struct c33_loadmap {
	unsigned short version, nsegs;
	struct { unsigned long addr, p_vaddr, p_memsz; } segs[];
};

/* Relocates without %r15, and so without data, calls or addresses. */
__attribute__((__used__, __noinline__))
static void *c33_fdpic_fixup(const struct c33_loadmap *map,
	unsigned long *a, unsigned long *z)
{
	int n = map->nsegs, i, j = 0, k = 0;
	unsigned long v;
	unsigned char *w;

	/* The list itself is in the text segment; its end is one past. */
	v = (unsigned long)a;
	for (i = 0; v - map->segs[i].p_vaddr >= map->segs[i].p_memsz; i++);
	w = (unsigned char *)(v - map->segs[i].p_vaddr + map->segs[i].addr);
	z = (unsigned long *)w + (z - a);
	a = (unsigned long *)w;

	for (;;) {
		v = *a;
		while (v - map->segs[j].p_vaddr >= map->segs[j].p_memsz)
			if (++j == n) j = 0;
		w = (unsigned char *)(v - map->segs[j].p_vaddr + map->segs[j].addr);
		if (++a == z) return w;
		/* .eh_frame keeps addresses at any byte; the C33 faults on a
		 * misaligned word. */
		if ((unsigned long)w & 3)
			v = w[0] | w[1]<<8 | w[2]<<16 | (unsigned long)w[3]<<24;
		else
			v = *(unsigned long *)w;
		/* The address's segment, or the one it is one past the end of. */
		for (i = 0; i < n; i++, k = k+1 < n ? k+1 : 0)
			if (v - map->segs[k].p_vaddr < map->segs[k].p_memsz)
				break;
		if (i == n)
			for (k = 0; k < n; k++)
				if (v - map->segs[k].p_vaddr == map->segs[k].p_memsz)
					break;
		if (k == n) {
			k = 0;
			continue;
		}
		v += map->segs[k].addr - map->segs[k].p_vaddr;
		if ((unsigned long)w & 3) {
			w[0] = v; w[1] = v>>8; w[2] = v>>16; w[3] = v>>24;
		} else {
			*(unsigned long *)w = v;
		}
	}
}

#ifndef SHARED

__asm__(
".text\n"
".global " START "\n"
".type " START ",@function\n"
START ":\n"
"	ld.w	%r0,%sp\n"
"	ld.w	%r1,%r6\n"
"	xld.w	%r7,__ROFIXUP_LIST__\n"
"	xld.w	%r8,__ROFIXUP_END__\n"
"	xcall	c33_fdpic_fixup\n"
"	ld.w	%r15,%r4\n"
"	ext	doff_hi(__c33_exec_loadmap)\n"
"	ext	doff_lo(__c33_exec_loadmap)\n"
"	ld.w	[%r15],%r1\n"
"	ld.w	%r4,%r0\n"
"	and	%r4,-16\n"
"	ld.w	%sp,%r4\n"
"	ld.w	%r6,%r0\n"
"	xcall	" START "_c\n"
"1:	jp	1b\n"
".size " START ",.-" START "\n"
".data\n"
".align 2\n"
".global __c33_exec_loadmap\n"
".hidden __c33_exec_loadmap\n"
".type __c33_exec_loadmap,@object\n"
".size __c33_exec_loadmap,4\n"
"__c33_exec_loadmap:\n"
"	.long 0\n"
".text\n"
);

#else

/* ld.so: _dlstart_c finds the load maps below the stack vector, the
 * program's at sp[-2] and ours at sp[-1], or zero when run as a program,
 * and our dynamic section in its second argument.  The stack moves below
 * them, 16-byte aligned, before the first call. */
__asm__(
".text\n"
".global " START "\n"
".type " START ",@function\n"
START ":\n"
"	ld.w	%r0,%sp\n"
"	ld.w	%r1,%r8\n"
"	ld.w	%r4,%sp\n"
"	sub	%r4,8\n"
"	ld.w	[%r4],%r6\n"
"	add	%r4,4\n"
"	ld.w	[%r4],%r7\n"
"	sub	%r4,4\n"
"	and	%r4,-16\n"
"	ld.w	%sp,%r4\n"
"	cmp	%r7,0\n"
"	jrne	1f\n"
"	ld.w	%r7,%r6\n"
"1:	ld.w	%r6,%r7\n"
"	xld.w	%r7,__ROFIXUP_LIST__\n"
"	xld.w	%r8,__ROFIXUP_END__\n"
"	xcall	c33_fdpic_fixup\n"
"	ld.w	%r15,%r4\n"
"	ld.w	%r6,%r0\n"
"	ld.w	%r7,%r1\n"
"	xcall	" START "_c\n"
"2:	jp	2b\n"
".size " START ",.-" START "\n"
);

#endif
