/* Self-relocation of a C33 FDPIC executable or of ld.so.
 *
 * Licensed under the LGPL v2.1, see the file COPYING.LIB in this tarball.
 *
 * Compiled into crtreloc.o, which the compiler driver links into every
 * executable, and into ld.so.  It runs before anything is relocated and
 * before %r15 is set, so it must use neither: no global data, no string
 * constants, no function addresses.  Its callers pass the link-time
 * addresses of __ROFIXUP_LIST__ and __ROFIXUP_END__, and the load map.
 */

#ifdef __FDPIC__

#include <sys/types.h>
#include <link.h>

/* A word at any byte address: .eh_frame keeps pointers wherever its
   encoding puts them, and the C33 faults on a misaligned word access.  */
struct __c33_unaligned_word { void *v; } __attribute__ ((packed));

/* The general case: any number of segments, through __reloc_pointer.  */
static void ***
__self_reloc_map (const struct elf32_fdpic_loadmap *map, void ***p, void ***e)
{
  for (; p < e; p++)
    {
      void **ptr = __reloc_pointer (*p, map);

      if ((unsigned long) ptr & 3)
	{
	  struct __c33_unaligned_word *u = (void *) ptr;

	  u->v = __reloc_pointer (u->v, map);
	}
      else
	*ptr = __reloc_pointer (*ptr, map);
    }
  return p;
}

/* Every word the list names holds a link-time address of this module; both
   the word and what it holds move with their segments.  The last entry is
   __dp's address, which is returned relocated: the caller's %r15.

   Every program runs this at every exec, a word at a time -- BusyBox has
   3,400 of them -- so a program of the usual two segments, text then data,
   gets a loop of its own.  The words are always in the data segment, which
   the linker checks; what they hold is in either, or one past the end of
   the data segment, as __reloc_pointer allows.  */
attribute_hidden void *
__self_reloc (const struct elf32_fdpic_loadmap *map, void ***p_link,
	      void ***e_link)
{
  void ***p = __reloc_pointer (p_link, map);
  /* The end is one past the text segment, which the load map would not
     translate.  */
  void ***e = p + (e_link - p_link);

  if (map->nsegs == 2)
    {
      unsigned long tv = map->segs[0].p_vaddr, tm = map->segs[0].p_memsz;
      unsigned long td = map->segs[0].addr - tv;
      unsigned long dv = map->segs[1].p_vaddr, dm = map->segs[1].p_memsz;
      unsigned long dd = map->segs[1].addr - dv;

      for (; p < e - 1; p++)
	{
	  unsigned long a = (unsigned long) *p, v;

	  if (a - dv >= dm || (a & 3))
	    {
	      p = __self_reloc_map (map, p, p + 1) - 1;
	      continue;
	    }
	  v = *(unsigned long *) (a + dd);
	  if (v - dv <= dm)
	    v += dd;
	  else if (v - tv < tm)
	    v += td;
	  else
	    v = (unsigned long) __reloc_pointer ((void *) v, map);
	  *(unsigned long *) (a + dd) = v;
	}
    }
  else
    p = __self_reloc_map (map, p, e - 1);

  if (p >= e)
    return (void *) -1;
  return __reloc_pointer (*p, map);
}

#endif /* __FDPIC__ */
