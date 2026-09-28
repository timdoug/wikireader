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

/* Every word the list names holds a link-time address of this module; both
   the word and what it holds move with their segments.  The last entry is
   __dp's address, which is returned relocated: the caller's %r15.  */
attribute_hidden void *
__self_reloc (const struct elf32_fdpic_loadmap *map, void ***p_link,
	      void ***e_link)
{
  void ***p = __reloc_pointer (p_link, map);
  /* The end is one past the text segment, which the load map would not
     translate.  */
  void ***e = p + (e_link - p_link);

  for (; p < e - 1; p++)
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
  if (p >= e)
    return (void *) -1;
  return __reloc_pointer (*p, map);
}

#endif /* __FDPIC__ */
