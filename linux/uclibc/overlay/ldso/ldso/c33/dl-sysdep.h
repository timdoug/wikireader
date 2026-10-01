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
#define R_C33_RELATIVE		34
#define R_C33_TLS_DTPMOD32	35
#define R_C33_TLS_DTPREL32	36
#define R_C33_TLS_TPREL32		37
#define R_C33_TLS_LE32		38

#define c33_tls_reloc_p(type) \
  ((type) == R_C33_TLS_DTPMOD32 || (type) == R_C33_TLS_DTPREL32 || \
   (type) == R_C33_TLS_TPREL32)
#define elf_machine_type_class(type) \
  ((c33_tls_reloc_p(type) || (type) == R_C33_FUNCDESC || \
    (type) == R_C33_FUNCDESC_VALUE) ? ELF_RTYPE_CLASS_PLT : 0)

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

/* A symbol lookup takes a remainder by its module's bucket count, and the
   PE core has no divide: libgcc's costs over a thousand cycles from SDRAM.
   With m = floor((2^32 - 1) / d), the high word of n * m is n / d or one
   less, so a multiply and one correction give the remainder.  m is kept
   for the last few bucket counts, which are per module.  */
static struct { unsigned long d, m; } c33_rem_cache[8] __attribute__((unused));

static __always_inline unsigned long
c33_urem (unsigned long n, unsigned long d)
{
	unsigned long r;
	unsigned int slot = d & 7;

	if (c33_rem_cache[slot].d != d) {
		c33_rem_cache[slot].d = d;
		c33_rem_cache[slot].m = d ? 0xffffffffUL / d : 0;
	}
	r = n - (unsigned long) (((unsigned long long) n
				  * c33_rem_cache[slot].m) >> 32) * d;
	return r >= d ? r - d : r;
}
#define do_rem(result, n, base) ((result) = c33_urem ((n), (base)))

#include "../fdpic/dl-sysdep.h"

static __always_inline Elf32_Addr
elf_machine_load_address (void)
{
	return 0;
}

/* Relocating an address through the load map searches it; a module in
   two segments, text below data, needs one comparison.  Every module the
   linker makes has two.  */
struct c33_segs {
	unsigned long split, delta0, delta1;
	int two;
};

static __always_inline void
c33_segs_init (struct c33_segs *s, const struct elf32_fdpic_loadmap *map)
{
	s->two = map->nsegs == 2 && map->segs[0].p_vaddr < map->segs[1].p_vaddr;
	if (s->two) {
		s->split = map->segs[1].p_vaddr;
		s->delta0 = map->segs[0].addr - map->segs[0].p_vaddr;
		s->delta1 = map->segs[1].addr - map->segs[1].p_vaddr;
	}
}

static __always_inline unsigned long
c33_segs_addr (const struct c33_segs *s, const struct elf32_fdpic_loadmap *map,
	       unsigned long a)
{
	if (s->two)
		return a + (a >= s->split ? s->delta1 : s->delta0);
	return (unsigned long) __reloc_pointer ((void *) a, map);
}

/* The linker puts a module's own addresses first in .rela.dyn, as
   R_C33_RELATIVE with the link-time address in the addend, and counts
   them in DT_RELACOUNT: most of a library's relocations, redone in every
   process.  A function of its own, so that the loop keeps its state in
   registers: a stack slot costs ten cycles a read from SDRAM.  */
static void __attribute__((noinline, unused))
_dl_c33_relative (struct elf32_fdpic_loadmap *map, const Elf32_Rela *rpnt,
		  const Elf32_Rela *end)
{
	struct c33_segs s;
	unsigned long split, delta0, delta1;

	c33_segs_init (&s, map);
	if (!s.two) {
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
	split = s.split;
	delta0 = s.delta0;
	delta1 = s.delta1;
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
