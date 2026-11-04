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

/* These relocate without %r15, and so without data or addresses, and
 * call only each other. */

/* Relocate the word at link-time address AT, by whichever segments it
 * and its contents are in; its contents may be one past the end of a
 * segment, and it may be at any byte (.eh_frame).  A value in no segment
 * is left alone. */
__attribute__((__used__, __noinline__))
static void c33_fdpic_fixup_one(const struct c33_loadmap *map, unsigned long at)
{
	int n = map->nsegs, i, k;
	unsigned long v;
	unsigned char *w;

	for (i = 0; at - map->segs[i].p_vaddr >= map->segs[i].p_memsz; i++);
	w = (unsigned char *)(at - map->segs[i].p_vaddr + map->segs[i].addr);
	if ((unsigned long)w & 3)
		v = w[0] | w[1]<<8 | w[2]<<16 | (unsigned long)w[3]<<24;
	else
		v = *(unsigned long *)w;
	for (k = 0; k < n; k++)
		if (v - map->segs[k].p_vaddr < map->segs[k].p_memsz)
			break;
	if (k == n)
		for (k = 0; k < n; k++)
			if (v - map->segs[k].p_vaddr == map->segs[k].p_memsz)
				break;
	if (k == n)
		return;
	v += map->segs[k].addr - map->segs[k].p_vaddr;
	if ((unsigned long)w & 3) {
		w[0] = v; w[1] = v>>8; w[2] = v>>16; w[3] = v>>24;
	} else {
		*(unsigned long *)w = v;
	}
}

/* Every word .rofixup lists, in the data segment, holds a link-time
 * address of this module; the last entry is __dp's own, which comes back
 * relocated: the caller's %r15.  BusyBox has 3,500 words, at every exec,
 * so a module of the usual two segments, text then data, has a loop of
 * its own, which keeps its state in registers; it hands anything else to
 * c33_fdpic_fixup_one. */
__attribute__((__used__, __noinline__, __optimize__("O2")))
static void *c33_fdpic_fixup(const struct c33_loadmap *map,
	unsigned long *a, unsigned long *z)
{
	unsigned long v = (unsigned long)a, at;
	int i;

	/* The list itself is in the text segment; its end is one past. */
	for (i = 0; v - map->segs[i].p_vaddr >= map->segs[i].p_memsz; i++);
	v += map->segs[i].addr - map->segs[i].p_vaddr;
	z = (unsigned long *)v + (z - a) - 1;
	a = (unsigned long *)v;

	if (map->nsegs == 2 && map->segs[0].p_vaddr < map->segs[1].p_vaddr) {
		unsigned long tv = map->segs[0].p_vaddr, tm = map->segs[0].p_memsz;
		unsigned long td = map->segs[0].addr - tv;
		unsigned long dv = map->segs[1].p_vaddr, dm = map->segs[1].p_memsz;
		unsigned long dd = map->segs[1].addr - dv;

		for (; a < z; a++) {
			unsigned long *w;

			at = *a;
			if (at - dv < dm && !(at & 3)) {
				w = (unsigned long *)(at + dd);
				v = *w;
				if (v - dv <= dm) {
					*w = v + dd;
					continue;
				}
				if (v - tv < tm) {
					*w = v + td;
					continue;
				}
			}
			c33_fdpic_fixup_one(map, at);
		}
	} else {
		for (; a < z; a++)
			c33_fdpic_fixup_one(map, *a);
	}

	/* __dp. */
	at = *a;
	for (i = 0; at - map->segs[i].p_vaddr >= map->segs[i].p_memsz; i++);
	return (void *)(at - map->segs[i].p_vaddr + map->segs[i].addr);
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
 * them, 16-byte aligned, before they are stored: an interrupt pushes its
 * PC and PSR on whatever stack is current, a program's included, over
 * anything below %sp. */
__asm__(
".text\n"
".global " START "\n"
".type " START ",@function\n"
START ":\n"
"	ld.w	%r0,%sp\n"
"	ld.w	%r1,%r8\n"
"	ld.w	%r4,%sp\n"
"	sub	%r4,8\n"
"	and	%r4,-16\n"
"	ld.w	%sp,%r4\n"
"	ld.w	%r4,%r0\n"
"	sub	%r4,8\n"
"	ld.w	[%r4],%r6\n"
"	add	%r4,4\n"
"	ld.w	[%r4],%r7\n"
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
