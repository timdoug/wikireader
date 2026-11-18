/*
 * Applies the R_68K_RELATIVE relocations of a static PIE before any GOT
 * entry is used: the m68k programs address their data through the GOT, and
 * a libc would otherwise do this. GCC loads even hidden symbols from the
 * GOT, so start-m68k.S computes the load base (the ELF header's run-time
 * address; the image is linked at 0) and _DYNAMIC PC-relative and passes
 * them in.
 */
struct dyn {
	long tag;
	unsigned long val;
};

struct rela {
	unsigned long offset;
	unsigned long info;
	long addend;
};

#define DT_RELA		7
#define DT_RELASZ	8
#define R_68K_RELATIVE	22

void self_relocate(const char *load_base, struct dyn *dynamic)
{
	unsigned long base = (unsigned long)load_base;
	unsigned long size = 0;
	struct rela *r = 0;
	struct dyn *d;

	for (d = dynamic; d->tag; d++) {
		if (d->tag == DT_RELA)
			r = (struct rela *)(base + d->val);
		else if (d->tag == DT_RELASZ)
			size = d->val;
	}
	for (; r && size >= sizeof(*r); size -= sizeof(*r), r++)
		if ((r->info & 0xff) == R_68K_RELATIVE)
			*(unsigned long *)(base + r->offset) = base + r->addend;
}
