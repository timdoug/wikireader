// SPDX-License-Identifier: GPL-2.0
/*
 * Self-relocation for a static FDPIC program with no C library, as musl's
 * crt1 does it (musl/overlay/arch/c33/crt_arch.h): the kernel enters with
 * the load map in %r6 and nothing relocated, and the entry passes the
 * link-time bounds of the words .rofixup lists.  Each holds a link-time
 * address of this program, and the last is __dp's own, which comes back
 * relocated: the caller's %r15.  Nothing here may use %r15, and so no
 * data or addresses, before that.
 */

struct loadmap {
	unsigned short version, nsegs;
	struct { unsigned long addr, p_vaddr, p_memsz; } segs[];
};

/* The word at link-time address AT, by whichever segments it and its
 * contents are in: its contents may be one past the end of a segment, and
 * it may be at any byte. */
static void fixup_one(const struct loadmap *map, unsigned long at)
{
	int n = map->nsegs, i, k;
	unsigned long v;
	unsigned char *w;

	for (i = 0; at - map->segs[i].p_vaddr >= map->segs[i].p_memsz; i++);
	w = (unsigned char *)(at - map->segs[i].p_vaddr + map->segs[i].addr);
	if ((unsigned long)w & 3)
		v = w[0] | w[1] << 8 | w[2] << 16 | (unsigned long)w[3] << 24;
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
		w[0] = v; w[1] = v >> 8; w[2] = v >> 16; w[3] = v >> 24;
	} else {
		*(unsigned long *)w = v;
	}
}

void *__self_reloc(const struct loadmap *map, unsigned long *a, unsigned long *z)
{
	unsigned long v = (unsigned long)a, at;
	int i;

	/* The list is in the text segment; its end is one past. */
	for (i = 0; v - map->segs[i].p_vaddr >= map->segs[i].p_memsz; i++);
	v += map->segs[i].addr - map->segs[i].p_vaddr;
	z = (unsigned long *)v + (z - a) - 1;
	a = (unsigned long *)v;

	for (; a < z; a++)
		fixup_one(map, *a);

	at = *a;
	for (i = 0; at - map->segs[i].p_vaddr >= map->segs[i].p_memsz; i++);
	return (void *)(at - map->segs[i].p_vaddr + map->segs[i].addr);
}
