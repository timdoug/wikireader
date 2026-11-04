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

#include "c33.h"

/* Target description features.  */

#include "../features/c33/core.c"
#include "../features/c33/fdpic.c"

target_desc_up
c33_create_target_description (const c33_features &features)
{
  target_desc_up tdesc = allocate_target_description ();

#ifndef IN_PROCESS_AGENT
  set_tdesc_architecture (tdesc.get (), "c33");
#endif

  long regnum = create_feature_c33_core (tdesc.get (), 0);
  if (features.fdpic_loadmaps)
    create_feature_c33_fdpic (tdesc.get (), regnum);

  return tdesc;
}

#ifndef GDBSERVER

const target_desc *
c33_lookup_target_description (const c33_features &features)
{
  static const_target_desc_up cache[2];
  const_target_desc_up &tdesc = cache[features.fdpic_loadmaps];

  if (tdesc == nullptr)
    tdesc = c33_create_target_description (features);
  return tdesc.get ();
}

#endif /* !GDBSERVER */

/* Every instruction is two bytes.  An ext prefix and the instruction it
   extends, and a delayed branch and the instruction in its slot, run as
   one: the hardware takes no interrupt between them, and nothing may stop
   there.  The PE core's branches are in class 0:

     bits 15-8 = 0x08-0x1f, low byte sign8: jr<cc> (0x08-0x1b, ten
       conditions), call (0x1c) and jp (0x1e), odd for the delayed forms;
       the target is the branch's own address plus twice the displacement,
       which ext prefixes widen: to 21 bits with one, and with two to 31,
       of the first only bits 12-3 taking part;
     bits 15-6, rb in bits 3-0: call %rb 0x18, ret 0x19, jp %rb 0x1a,
       jpr %rb 0x0b (PC-relative), and delayed 0x1c, 0x1d, 0x1e, 0x0f.  */

std::vector<CORE_ADDR>
c33_get_next_pcs
  (CORE_ADDR pc,
   gdb::function_view<ULONGEST (CORE_ADDR addr, int len)> read_memory,
   gdb::function_view<ULONGEST (int regnum)> read_register)
{
  unsigned int ext[2];
  int n_ext = 0;
  unsigned int insn;
  CORE_ADDR at = pc;

  for (;;)
    {
      insn = read_memory (at, 2);
      if ((insn >> 13) != 6 || n_ext == 2)
	break;
      ext[n_ext++] = insn & 0x1fff;
      at += 2;
    }

  unsigned int op8 = insn >> 8, op10 = insn >> 6;
  CORE_ADDR next = at + 2;

  if (op8 >= 0x08 && op8 <= 0x1f)
    {
      uint32_t v = insn & 0xff;
      unsigned int width = 8;

      if (n_ext == 1)
	{
	  v |= ext[0] << 8;
	  width = 21;
	}
      else if (n_ext == 2)
	{
	  v |= ((ext[0] >> 3) << 21) | (ext[1] << 8);
	  width = 31;
	}
      v &= (1u << width) - 1;
      if (v & (1u << (width - 1)))
	v |= ~((1u << width) - 1);

      CORE_ADDR target = (at + ((int32_t) v << 1)) & 0xffffffff;

      if (op8 & 1)
	next = at + 4;
      if (op8 >= 0x1c)
	return { target };
      return { next, target };
    }

  if ((insn >> 13) == 0)
    {
      ULONGEST rb = read_register (C33_R0_REGNUM + (insn & 0xf));

      switch (op10)
	{
	case 0x18: case 0x1c:	/* call %rb */
	case 0x1a: case 0x1e:	/* jp %rb */
	  return { rb & ~(CORE_ADDR) 1 };
	case 0x0b: case 0x0f:	/* jpr %rb */
	  return { (at + (rb & ~(ULONGEST) 1)) & 0xffffffff };
	case 0x19: case 0x1d:	/* ret */
	  return { read_memory (read_register (C33_SP_REGNUM), 4)
		   & ~(CORE_ADDR) 1 };
	}
    }

  return { next };
}
