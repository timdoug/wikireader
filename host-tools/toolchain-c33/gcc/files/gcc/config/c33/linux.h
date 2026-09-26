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

/* Every program is a static bFLT for the WikiReader's PE core, with its
   text shared between processes: text holds no absolute address, %r15
   points at the data segment, and calls may span the whole image.  The
   linker converts the result to bFLT when given -elf2flt, as on every
   other no-MMU Linux port.  */

#undef DRIVER_SELF_SPECS
#define DRIVER_SELF_SPECS				\
  "%{!mcore=*:-mcore=c33pe}",				\
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

/* uClibc's startup code, with no crtbegin/crtend: constructors run from
   .init_array, which the linker script collects.  */
#undef STARTFILE_SPEC
#define STARTFILE_SPEC "crt1.o%s crti.o%s"

#undef ENDFILE_SPEC
#define ENDFILE_SPEC "crtn.o%s"

#undef LIB_SPEC
#define LIB_SPEC "%{pthread:-lpthread} -lc"

/* There are no shared libraries, so every link is static and libc and
   libgcc resolve each other's references.  */
#undef LINK_SPEC
#define LINK_SPEC "%{shared:%e-shared is not supported on no-MMU C33 Linux} \
  %{!r:-static}"

#undef LINK_GCC_C_SEQUENCE_SPEC
#define LINK_GCC_C_SEQUENCE_SPEC "--start-group %G %{!nolibc:%L} --end-group"

#undef LINK_EH_SPEC
