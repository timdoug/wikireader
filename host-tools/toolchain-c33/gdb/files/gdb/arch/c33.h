/* Common target-dependent functionality for the Epson S1C33.

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

#ifndef GDB_ARCH_C33_H
#define GDB_ARCH_C33_H

#include "gdbsupport/function-view.h"
#include "gdbsupport/tdesc.h"
#include <vector>

/* Register numbers, in the order of the target description's
   org.gnu.gdb.c33.core and org.gnu.gdb.c33.fdpic features.  The first
   nineteen are also GCC's DWARF numbers.  */

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
  C33_NUM_CORE_REGS = 21,
  C33_LOADMAP_EXEC_REGNUM = 21,
  C33_LOADMAP_INTERP_REGNUM = 22,
  C33_NUM_REGS = 23
};

/* The C33's breakpoint, int 3.  Linux reports the PC at it.  */
#define C33_BREAKPOINT_INSN 0x0483

/* What a target description has beyond the core registers.  */

struct c33_features
{
  /* The FDPIC load map addresses, which a core file records.  */
  bool fdpic_loadmaps = false;

  bool operator== (const c33_features &other) const = default;
};

/* Create a target description with FEATURES.  */

extern target_desc_up c33_create_target_description
  (const c33_features &features);

#ifndef GDBSERVER
/* The cached description with FEATURES, which lives as long as GDB.  */

extern const target_desc *c33_lookup_target_description
  (const c33_features &features);
#endif

/* Where the instruction at PC can go next: GDB and gdbserver put a
   breakpoint at each to step it, there being no hardware single step.
   READ_MEMORY reads LEN bytes of code as an unsigned integer and
   READ_REGISTER a register by its c33_regnum.  */

extern std::vector<CORE_ADDR> c33_get_next_pcs
  (CORE_ADDR pc,
   gdb::function_view<ULONGEST (CORE_ADDR addr, int len)> read_memory,
   gdb::function_view<ULONGEST (int regnum)> read_register);

#endif /* GDB_ARCH_C33_H */
