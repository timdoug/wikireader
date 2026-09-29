/* C33 FDPIC dynamic linker: system-dependent definitions.
 *
 * Licensed under the LGPL v2.1, see the file COPYING.LIB in this tarball.
 *
 * The ABI is in host-tools/toolchain-c33/gcc/ABI.md and the linker's side
 * in bfd/elf32-c33.c: each module reaches its data segment through %r15,
 * which a function descriptor, {entry, %r15}, carries between modules.
 *
 * A .plt descriptor is bound when first called (resolve.S): until then it
 * points at a stub of the module's .plt.  The two words cannot be read
 * together, so a caller may read the stub with the new %r15; the stub
 * therefore names its module by its own address, never by %r15, and every
 * module's .got header holds the resolver's descriptor, which INIT_GOT
 * (../fdpic/dl-sysdep.h) puts there with the module in its third word.
 */

#define ELF_USES_RELOCA

#define DL_NO_COPY_RELOCS

#ifndef EM_SE_C33
#define EM_SE_C33 107
#endif
#define MAGIC1 EM_SE_C33
#undef  MAGIC2

#define ELF_TARGET "C33"

#define EF_C33_FDPIC		0x00000001

#define R_C33_NONE		0
#define R_C33_32		1
#define R_C33_FUNCDESC		32
#define R_C33_FUNCDESC_VALUE	33

/* ld.so relocates itself with its own dynamic relocations.  */
#define ARCH_NEEDS_BOOTSTRAP_RELOCS

/* Before ld.so has relocated itself the address of a message would be
   wrong, and dl-string.h leaves SEND_STDERR empty on FDPIC anyway.  */
#undef SEND_EARLY_STDERR
#define SEND_EARLY_STDERR(S) do { } while (0)

/* Only FDPIC shared libraries, whose segments go anywhere.  */
#define DL_CHECK_LIB_TYPE(epnt, piclib, _dl_progname, libname) \
do \
{ \
  if ((epnt)->e_flags & EF_C33_FDPIC) \
    (piclib) = 2; \
  else \
    { \
      _dl_internal_error_number = LD_ERROR_NOTDYN; \
      _dl_dprintf(2, "%s: '%s' is not an FDPIC shared library" \
		  "\n", (_dl_progname), (libname)); \
      _dl_close(infile); \
      return NULL; \
    } \
} \
while (0)

extern int _dl_linux_resolve(void) __attribute__((__visibility__("hidden")));

#include "../fdpic/dl-sysdep.h"

static __always_inline Elf32_Addr
elf_machine_load_address (void)
{
	return 0;
}

/* The linker puts a module's own addresses first in .rela.dyn, as R_C33_32
   against no symbol with the link-time address in the addend, and counts
   them in DT_RELACOUNT: most of a library's relocations, redone in every
   process.  Relocating an address through the load map searches it; a
   module in two segments, text below data, needs one comparison.  A
   function of its own, so that the loop keeps its state in registers:
   a stack slot costs ten cycles a read from SDRAM.  */
static void __attribute__((noinline, unused))
_dl_c33_relative (struct elf32_fdpic_loadmap *map, const Elf32_Rela *rpnt,
		  const Elf32_Rela *end)
{
	unsigned long split, delta0, delta1;

	if (map->nsegs != 2 || map->segs[0].p_vaddr >= map->segs[1].p_vaddr) {
		/* Not seen: every module the linker makes has two.  */
		for (; rpnt < end; rpnt++) {
			unsigned long a[2] = { rpnt->r_offset, rpnt->r_addend };
			int i, j;

			for (i = 0; i < 2; i++)
				for (j = 0; j < map->nsegs; j++)
					if (a[i] - map->segs[j].p_vaddr
					    <= map->segs[j].p_memsz) {
						a[i] += map->segs[j].addr
							- map->segs[j].p_vaddr;
						break;
					}
			((struct { unsigned long v; } __attribute__((packed)) *)
			 a[0])->v = a[1];
		}
		return;
	}
	split = map->segs[1].p_vaddr;
	delta0 = map->segs[0].addr - map->segs[0].p_vaddr;
	delta1 = map->segs[1].addr - map->segs[1].p_vaddr;
	for (; rpnt < end; rpnt++) {
		unsigned long off = rpnt->r_offset, v = rpnt->r_addend;

		off += off >= split ? delta1 : delta0;
		v += v >= split ? delta1 : delta0;
		/* .eh_frame keeps pointers at any byte address.  */
		if (off & 3)
			((struct { unsigned long v; } __attribute__((packed)) *)
			 off)->v = v;
		else
			*(unsigned long *) off = v;
	}
}

static __always_inline void
elf_machine_relative (DL_LOADADDR_TYPE load_off, const Elf32_Addr rel_addr,
		      Elf32_Word relative_count)
{
	_dl_c33_relative (load_off.map, (const Elf32_Rela *) rel_addr,
			  (const Elf32_Rela *) rel_addr + relative_count);
}
