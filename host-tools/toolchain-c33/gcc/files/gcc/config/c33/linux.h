/* Definitions for C33 no-MMU Linux (uClinux) with uClibc-ng or musl.
   Copyright (C) 2026 Free Software Foundation, Inc.

   This file is part of GCC.

   GCC is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published
   by the Free Software Foundation; either version 3, or (at your
   option) any later version.

   GCC is distributed in the hope that it will be useful, but WITHOUT
   ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
   or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
   License for more details.

   Under Section 7 of GPL version 3, you are granted additional
   permissions described in the GCC Runtime Library Exception, version
   3.1, as published by the Free Software Foundation.

   You should have received a copy of the GNU General Public License and
   a copy of the GCC Runtime Library Exception along with this program;
   see the files COPYING3 and COPYING.RUNTIME respectively.  If not, see
   <http://www.gnu.org/licenses/>.  */

/* Every program is FDPIC ELF for the WikiReader's PE core: each module --
   the executable and every shared library -- has its text shared between
   processes and its own data segment, which its code reaches through %r15.
   Calls to other modules go through function descriptors (see c33.md).
   -mno-fdpic builds static -msep-data code, as ARM's FDPIC target keeps
   its non-FDPIC mode.

   Calls are short by default: a direct call never leaves its module (one
   in another module goes through the module's own .plt), and a short call
   reaches 2 MB either way, twice the largest module.  ld reports a call
   out of reach; a module that big needs -mlong-calls.  */

/* The Linux assembler makes FDPIC objects by default, so non-FDPIC code
   (-mno-fdpic, -medda32) says so.  */
#undef ASM_FDPIC_SPEC
#define ASM_FDPIC_SPEC "%{mfdpic:-mfdpic;:-mno-fdpic}"

/* uClibc-ng as built here (DO_C99_MATH) has C94's and C99's library
   functions and sincos, but not all of C99's complex ones (csqrt) or C11's
   aligned_alloc: what GCC calls a BSD libc.  linux.h assumes a uClibc has
   none, which stops GCC folding to round, exp2 and the like.  musl has
   them all, which linux.h knows.  */
#if DEFAULT_LIBC != LIBC_MUSL
#undef TARGET_LIBC_HAS_FUNCTION
#define TARGET_LIBC_HAS_FUNCTION bsd_libc_has_function
#endif

/* The dynamic linker, and the object with which an executable relocates
   itself: uClibc-ng's crtreloc.o, or for musl, crt1.o itself.  */
#if DEFAULT_LIBC == LIBC_MUSL
#define C33_DYNAMIC_LINKER "/lib/ld-musl-c33.so.1"
#define C33_SELF_RELOC_SPEC ""
#else
#define C33_DYNAMIC_LINKER "/lib/ld-uClibc.so.0"
#define C33_SELF_RELOC_SPEC "%{!mno-fdpic:crtreloc.o%s}"
#endif

/* Thread-local common data, which ld places in .tbss.  */
#undef ASM_OUTPUT_TLS_COMMON
#define ASM_OUTPUT_TLS_COMMON(FILE, DECL, NAME, SIZE)			\
  do									\
    {									\
      fputs ("\t.tls_common\t", (FILE));				\
      assemble_name ((FILE), (NAME));					\
      fprintf ((FILE), "," HOST_WIDE_INT_PRINT_UNSIGNED ",%u\n",	\
	       (SIZE), DECL_ALIGN (DECL) / BITS_PER_UNIT);		\
    }									\
  while (0)

#undef DRIVER_SELF_SPECS
#define DRIVER_SELF_SPECS				\
  "%{!mcore=*:-mcore=c33pe}",				\
  "%{!mno-fdpic:%{!medda32:-mfdpic}}",			\
  "%{!mno-sep-data:%{!medda32:-msep-data}}"

#undef TARGET_OS_CPP_BUILTINS
#define TARGET_OS_CPP_BUILTINS()			\
  do							\
    {							\
      GNU_USER_TARGET_OS_CPP_BUILTINS ();		\
      /* No MMU, so no fork: packages test this.  */	\
      builtin_define ("__uClinux__");			\
    }							\
  while (0)

#undef CPP_SPEC
#define CPP_SPEC "%{posix:-D_POSIX_SOURCE} %{pthread:-D_REENTRANT}"

/* The C library's startup code, and what an executable relocates itself
   with before anything else.  Constructors run from
   .init_array, which the linker script collects. Static crtbeginT registers
   .eh_frame; dynamic modules use PT_GNU_EH_FRAME discovery. */
#undef STARTFILE_SPEC
#define STARTFILE_SPEC \
  "%{!shared:crt1.o%s " C33_SELF_RELOC_SPEC "} crti.o%s \
   %{shared:crtbeginS.o%s;static:crtbeginT.o%s;:crtbegin.o%s}"

#undef ENDFILE_SPEC
#define ENDFILE_SPEC "%{shared:crtendS.o%s;:crtend.o%s} crtn.o%s"

/* Exceptions unwind with DWARF tables, in shared text: they reach code
   PC-relative and data relative to __dp (c33.h).  A dynamically linked
   module's tables are found through PT_GNU_EH_FRAME and its load map,
   which gives __dp; a static program's crtbeginT registers them with
   __dp, which is %r15.  */
#undef DWARF2_UNWIND_INFO
#define DWARF2_UNWIND_INFO 1
#define CRT_GET_RFIB_DATA(BASE) __asm__ ("ld.w %0,%%r15" : "=r" (BASE))

/* crtstuff.c turns PT_GNU_EH_FRAME off for every uClibc, and for any C
   library but glibc; uClibc-ng's and musl's dl_iterate_phdr give the load
   map it needs.  crtbeginT.o, for static programs, keeps registering its
   tables.  */
#if (defined CRT_BEGIN || defined CRT_END) && defined HAVE_LD_EH_FRAME_HDR \
    && !defined CRTSTUFFT_O
#define USE_PT_GNU_EH_FRAME
#endif

/* uClibc-ng's libc holds the thread library, as musl's does, so libgcc
   calls pthreads directly.  A weak reference would decide a program was single-threaded
   whenever nothing else had pulled in the object it names.  */
#define GTHREAD_USE_WEAK 0

#undef C33_NATIVE_TLS
#define C33_NATIVE_TLS 1
#define TARGET_DL_ITERATE_PHDR 1

/* Every object says whether its code needs an executable stack.  */
#undef TARGET_ASM_FILE_END
#define TARGET_ASM_FILE_END file_end_indicate_exec_stack

#undef LIB_SPEC
#define LIB_SPEC "%{pthread:-lpthread} -lc"

/* The FDPIC emulation, and ld.so unless the link is static.  Without an
   MMU a program's stack cannot grow: the kernel allocates the size in
   PT_GNU_STACK whole at exec, or 128 KB if that is zero.  32 KB here, and
   a later -Wl,-z,stack-size= overrides it.  16 KB is too little for
   Xlib clients -- a lazily bound first call adds
   ld.so's resolver at whatever depth it happens -- and an overflow writes
   silently into whatever lies below the stack.  */
#undef LINK_SPEC
#define LINK_SPEC "%{mno-fdpic:-m c33 %{shared:%e-shared needs -mfdpic} %{!r:-static}} \
  %{!mno-fdpic:-m c33fdpic %{shared} %{static} \
    %{!shared:%{!static:%{rdynamic:-export-dynamic} \
      -dynamic-linker " C33_DYNAMIC_LINKER "}} \
    %{!r:--eh-frame-hdr %{!shared:-z noexecstack -z stack-size=32768}}}"

#undef LINK_GCC_C_SEQUENCE_SPEC
#define LINK_GCC_C_SEQUENCE_SPEC "--start-group %G %{!nolibc:%L} --end-group"

#undef LINK_EH_SPEC
