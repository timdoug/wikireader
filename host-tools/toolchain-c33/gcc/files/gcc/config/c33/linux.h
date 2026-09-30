/* Definitions for C33 no-MMU Linux (uClinux) with uClibc-ng.
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
   its non-FDPIC mode.  */

#undef DRIVER_SELF_SPECS
#define DRIVER_SELF_SPECS				\
  "%{!mcore=*:-mcore=c33pe}",				\
  "%{!mno-fdpic:%{!medda32:-mfdpic}}",			\
  "%{!mno-sep-data:%{!medda32:-msep-data}}",		\
  "%{!mno-long-calls:-mlong-calls}"

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

/* uClibc's startup code, and crtreloc.o, with which an executable
   relocates itself before anything else.  Constructors run from
   .init_array, which the linker script collects. Static crtbeginT registers
   .eh_frame; dynamic modules use PT_GNU_EH_FRAME discovery. */
#undef STARTFILE_SPEC
#define STARTFILE_SPEC \
  "%{!shared:crt1.o%s %{!mno-fdpic:crtreloc.o%s}} crti.o%s \
   %{shared:crtbeginS.o%s;static:crtbeginT.o%s;:crtbegin.o%s}"

#undef ENDFILE_SPEC
#define ENDFILE_SPEC "%{shared:crtendS.o%s;:crtend.o%s} crtn.o%s"

/* Exceptions unwind with DWARF tables. The linker script keeps .eh_frame and
   .gcc_except_table in the data segment, where the loader relocates their
   absolute pointers.  */
#undef DWARF2_UNWIND_INFO
#define DWARF2_UNWIND_INFO 1

/* uClibc-ng's libc holds the thread library, so libgcc calls pthreads
   directly.  A weak reference would decide a program was single-threaded
   whenever nothing else had pulled in the object it names.  */
#define GTHREAD_USE_WEAK 0

#undef C33_NATIVE_TLS
#define C33_NATIVE_TLS 1
/* Configure's generic assembler probe cannot recognize C33 instructions. */
#define HAVE_LD_EH_FRAME_HDR 1
#define TARGET_DL_ITERATE_PHDR 1

#undef LIB_SPEC
#define LIB_SPEC "%{pthread:-lpthread} -lc"

/* Keep arithmetic helpers static, but share the optional unwinder between
   libc, C++ and dynamically loaded modules. libgcc_eh.a
   is installed by the post-libc shared-runtime build for static links. */
#undef LIBGCC_SPEC
#define LIBGCC_SPEC \
  "-lgcc %{static|static-libgcc:-lgcc_eh;shared-libgcc:-lgcc_s;:--push-state --as-needed -lgcc_s --pop-state}"

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
      -dynamic-linker /lib/ld-uClibc.so.0}} \
    %{!r:--eh-frame-hdr %{!shared:-z noexecstack -z stack-size=32768}}}"

#undef LINK_GCC_C_SEQUENCE_SPEC
#define LINK_GCC_C_SEQUENCE_SPEC "--start-group %G %{!nolibc:%L} --end-group"

#undef LINK_EH_SPEC
