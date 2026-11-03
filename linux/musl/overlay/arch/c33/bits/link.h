typedef uint32_t Elf_Symndx;

#ifdef __FDPIC__
/* A module's segments go anywhere: the kernel and ld.so describe where
 * with a load map, and dl_iterate_phdr reports a module by its load map
 * and its %r15, which libgcc's unwinder translates addresses with. */
struct elf32_fdpic_loadseg {
	Elf32_Addr addr;
	Elf32_Addr p_vaddr;
	Elf32_Word p_memsz;
};

struct elf32_fdpic_loadmap {
	Elf32_Half version;
	Elf32_Half nsegs;
	struct elf32_fdpic_loadseg segs[];
};

struct elf32_fdpic_loadaddr {
	struct elf32_fdpic_loadmap *map;
	void *got_value;
};

#define __DLPI_ADDR_TYPE struct elf32_fdpic_loadaddr

/* The run-time address of link-time address P, which may be one past the
 * end of its segment. */
static __inline void *__reloc_pointer(void *__p,
	const struct elf32_fdpic_loadmap *__map)
{
	int __c;
	for (__c = 0; __c < __map->nsegs; __c++) {
		unsigned long __o = (unsigned long)__p - __map->segs[__c].p_vaddr;
		if (__o <= __map->segs[__c].p_memsz)
			return (char *)__map->segs[__c].addr + __o;
	}
	return (void *)-1;
}
#define __RELOC_POINTER(ptr, loadaddr) \
	(__reloc_pointer((void *)(ptr), (loadaddr).map))
#endif
