/* GNU/Linux on the Epson S1C33 interface for the GDB server.

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

/* The registers come from NT_PRSTATUS, the kernel's struct pt_regs, and
   go out in GDB's order (c33-linux.dat).  There is no hardware single
   step: GDB steps by breakpoints, and a breakpoint is int 3, which the
   kernel reports at its own address.  The FDPIC load maps come from
   linux-low's PTRACE_GETFDPIC support.  */

#include "linux-low.h"
#include "elf/common.h"
#include "nat/gdb_ptrace.h"
#include "tdesc.h"

class c33_target : public linux_process_target
{
public:
  const regs_info *get_regs_info () override;
  const gdb_byte *sw_breakpoint_from_kind (int kind, int *size) override;

protected:
  void low_arch_setup () override;
  bool low_cannot_fetch_register (int regno) override;
  bool low_cannot_store_register (int regno) override;
  bool low_supports_breakpoints () override;
  CORE_ADDR low_get_pc (regcache *regcache) override;
  void low_set_pc (regcache *regcache, CORE_ADDR newpc) override;
  bool low_breakpoint_at (CORE_ADDR pc) override;
};

static c33_target the_c33_target;

void init_registers_c33_linux (void);
extern const_target_desc_up tdesc_c33_linux;

/* GDB's registers r0-r15, sp, alr, ahr, psr and pc, by their word in
   struct pt_regs: r[16], alr, ahr, sp, vector, orig_r4, reserved, psr,
   pc.  */

enum { C33_NUM_REGS = 21, C33_PT_REGS_WORDS = 24 };

static const int c33_pt_regs_word[C33_NUM_REGS] =
{
  0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
  18,	/* sp */
  16,	/* alr */
  17,	/* ahr */
  22,	/* psr */
  23,	/* pc */
};

static void
c33_fill_gregset (struct regcache *regcache, void *buf)
{
  gdb_byte *regs = (gdb_byte *) buf;

  for (int i = 0; i < C33_NUM_REGS; i++)
    collect_register (regcache, i, regs + 4 * c33_pt_regs_word[i]);
}

static void
c33_store_gregset (struct regcache *regcache, const void *buf)
{
  const gdb_byte *regs = (const gdb_byte *) buf;

  for (int i = 0; i < C33_NUM_REGS; i++)
    supply_register (regcache, i, regs + 4 * c33_pt_regs_word[i]);
}

static struct regset_info c33_regsets[] =
{
  { PTRACE_GETREGSET, PTRACE_SETREGSET, NT_PRSTATUS,
    C33_PT_REGS_WORDS * 4, GENERAL_REGS,
    c33_fill_gregset, c33_store_gregset },
  NULL_REGSET
};

static struct regsets_info c33_regsets_info =
{
  c33_regsets,	/* regsets */
  0,		/* num_regsets */
  NULL,		/* disabled_regsets */
};

/* No PTRACE_PEEKUSR: every register is in the regset.  */

static struct regs_info c33_regs_info =
{
  NULL,		/* regset_bitmap */
  NULL,		/* usrregs */
  &c33_regsets_info
};

const regs_info *
c33_target::get_regs_info ()
{
  return &c33_regs_info;
}

void
c33_target::low_arch_setup ()
{
  current_process ()->tdesc = tdesc_c33_linux.get ();
}

/* There is no PTRACE_PEEKUSR, and no usrregs: every register comes
   through the regset.  */

bool
c33_target::low_cannot_fetch_register (int regno)
{
  return true;
}

bool
c33_target::low_cannot_store_register (int regno)
{
  return true;
}

bool
c33_target::low_supports_breakpoints ()
{
  return true;
}

CORE_ADDR
c33_target::low_get_pc (regcache *regcache)
{
  return linux_get_pc_32bit (regcache);
}

void
c33_target::low_set_pc (regcache *regcache, CORE_ADDR pc)
{
  linux_set_pc_32bit (regcache, pc);
}

/* int 3.  */
static const gdb_byte c33_breakpoint[] = { 0x83, 0x04 };

const gdb_byte *
c33_target::sw_breakpoint_from_kind (int kind, int *size)
{
  *size = sizeof c33_breakpoint;
  return c33_breakpoint;
}

bool
c33_target::low_breakpoint_at (CORE_ADDR where)
{
  gdb_byte insn[sizeof c33_breakpoint];

  read_memory (where, insn, sizeof insn);
  return memcmp (insn, c33_breakpoint, sizeof insn) == 0;
}

linux_process_target *the_linux_target = &the_c33_target;

void
initialize_low_arch (void)
{
  init_registers_c33_linux ();
  initialize_regsets_info (&c33_regsets_info);
}
