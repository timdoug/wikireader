/* The C33 Linux thread pointer: a uniprocessor without an MMU publishes the
   current thread's TP in this aligned word of A0 RAM, so no general
   register is reserved for it.  Part of the Linux ABI; it matches the
   kernel's <asm/tls.h>.

   Copyright (C) 2026 Free Software Foundation, Inc.

   This file is part of GCC.

   GCC is free software; you can redistribute it and/or modify it under
   the terms of the GNU General Public License as published by the Free
   Software Foundation; either version 3, or (at your option) any later
   version.

   GCC is distributed in the hope that it will be useful, but WITHOUT ANY
   WARRANTY; without even the implied warranty of MERCHANTABILITY or
   FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
   for more details.

   You should have received a copy of the GNU General Public License
   along with GCC; see the file COPYING3.  If not see
   <http://www.gnu.org/licenses/>.  */

#ifndef GCC_C33_LINUX_TLS_H
#define GCC_C33_LINUX_TLS_H

#define C33_TLS_SLOT_ADDRESS 0x00001fbc

#endif /* GCC_C33_LINUX_TLS_H */
