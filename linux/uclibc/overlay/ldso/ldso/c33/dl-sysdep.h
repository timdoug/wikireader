/* C33 FDPIC dynamic linker: system-dependent definitions.
 *
 * Licensed under the LGPL v2.1, see the file COPYING.LIB in this tarball.
 *
 * The ABI is in host-tools/toolchain-c33/gcc/ABI.md and the linker's side
 * in bfd/elf32-c33.c: each module reaches its data segment through %r15,
 * which a function descriptor, {entry, %r15}, carries between modules.
 * Descriptors are resolved when a module is loaded; there is no lazy
 * binding, because nothing makes the descriptor's two words change
 * together.
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

#include "../fdpic/dl-sysdep.h"

/* The module's %r15 is its DT_PLTGOT, __dp, where the linker leaves
   sixteen bytes for ld.so.  The third word holds the module, which
   _dl_funcdesc_for finds from a %r15.  With no lazy binding there is no
   resolver descriptor to store in the first two.  */
#undef INIT_GOT
#define INIT_GOT(GOT_BASE, MODULE) \
do { \
  (MODULE)->loadaddr.got_value = (void *) (GOT_BASE); \
  (GOT_BASE)[2] = (unsigned long) (MODULE); \
} while (0)

static __always_inline Elf32_Addr
elf_machine_load_address (void)
{
	return 0;
}

static __always_inline void
elf_machine_relative (DL_LOADADDR_TYPE load_off, const Elf32_Addr rel_addr,
		      Elf32_Word relative_count)
{
}
