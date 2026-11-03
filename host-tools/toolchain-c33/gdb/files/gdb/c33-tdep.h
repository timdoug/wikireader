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

/* GDB's register numbers.  The first nineteen are also GCC's DWARF
   numbers.  The last two are FDPIC's: the addresses of the executable's
   and the interpreter's load maps, which place the program in memory, as
   FR-V's are.  */

enum c33_regnum
{
  C33_R0_REGNUM = 0,
  C33_R4_REGNUM = 4,		/* return value, low word */
  C33_R5_REGNUM = 5,		/* return value, high word */
  C33_R6_REGNUM = 6,		/* first argument */
  C33_R15_REGNUM = 15,		/* the data segment pointer, __dp */
  C33_SP_REGNUM = 16,
  C33_ALR_REGNUM = 17,
  C33_AHR_REGNUM = 18,
  C33_PSR_REGNUM = 19,
  C33_PC_REGNUM = 20,
  C33_LOADMAP_EXEC_REGNUM = 21,
  C33_LOADMAP_INTERP_REGNUM = 22,
  C33_NUM_REGS = 23
};

/* The DWARF column GCC uses for the return address: there is no register
   holding it, so it is one past the compiler's registers.  */
#define C33_DWARF_RETURN_COLUMN 22

/* The C33's breakpoint, int 3.  The kernel reports the PC at it.  */
#define C33_BREAKPOINT_INSN 0x0483

/* Fetch the addresses of the interpreter's and the executable's FDPIC
   load maps, either of which may be null.  Return 0 on success, -1 if
   the program does not use FDPIC.  */
extern int c33_fdpic_loadmap_addresses (struct gdbarch *gdbarch,
					CORE_ADDR *interp_addr,
					CORE_ADDR *exec_addr);

#endif /* GDB_C33_TDEP_H */
