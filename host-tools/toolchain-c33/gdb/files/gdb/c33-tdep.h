/* Target-dependent code for the Epson S1C33, for GDB.

   Copyright (C) 2026 Free Software Foundation, Inc.

   This file is part of GDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#ifndef GDB_C33_TDEP_H
#define GDB_C33_TDEP_H

#include "gdbarch.h"
#include "arch/c33.h"

struct c33_gdbarch_tdep : gdbarch_tdep_base
{
  /* Whether the program is FDPIC, and so has function descriptors.  */
  bool fdpic = false;

  /* Whether the target description has the FDPIC load map addresses,
     registers C33_LOADMAP_EXEC_REGNUM and C33_LOADMAP_INTERP_REGNUM.  */
  bool loadmap_regs = false;
};

/* The registers are numbered as in the target description (arch/c33.h),
   the core's also as GCC's DWARF numbers, except for the return address:
   no register holds it, so its column is one past the compiler's.  */
#define C33_DWARF_RETURN_COLUMN 22

/* Fetch the addresses of the interpreter's and the executable's FDPIC
   load maps, either of which may be null.  Return 0 on success, -1 if
   the program does not use FDPIC.  */
extern int c33_fdpic_loadmap_addresses (struct gdbarch *gdbarch,
					CORE_ADDR *interp_addr,
					CORE_ADDR *exec_addr);

#endif /* GDB_C33_TDEP_H */
