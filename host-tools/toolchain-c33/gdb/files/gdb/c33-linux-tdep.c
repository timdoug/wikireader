/* Target-dependent code for no-MMU GNU/Linux on the Epson S1C33, for GDB.

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

#include "elf-bfd.h"
#include "elf/c33.h"
#include "linux-tdep.h"
#include "osabi.h"
#include "regcache.h"
#include "regset.h"
#include "c33-tdep.h"
#include "solib-c33.h"

/* The kernel's struct pt_regs, which is NT_PRSTATUS's pr_reg, and after
   it the two load map addresses of struct elf_prstatus_fdpic, which BFD
   includes in a core file's .reg, as FR-V's does.  */

static const struct regcache_map_entry c33_linux_gregmap[] =
  {
    { 16, C33_R0_REGNUM, 4 },
    { 1, C33_ALR_REGNUM, 4 },
    { 1, C33_AHR_REGNUM, 4 },
    { 1, C33_SP_REGNUM, 4 },
    { 1, REGCACHE_MAP_SKIP, 4 },	/* vector */
    { 1, REGCACHE_MAP_SKIP, 4 },	/* orig_r4 */
    { 1, REGCACHE_MAP_SKIP, 4 },	/* reserved */
    { 1, C33_PSR_REGNUM, 4 },
    { 1, C33_PC_REGNUM, 4 },
    { 1, C33_LOADMAP_EXEC_REGNUM, 4 },
    { 1, C33_LOADMAP_INTERP_REGNUM, 4 },
    { 0 }
  };

#define C33_LINUX_SIZEOF_GREGSET ((24 + 2) * 4)

static const struct regset c33_linux_gregset =
  {
    c33_linux_gregmap, regcache_supply_regset, regcache_collect_regset
  };

static void
c33_linux_iterate_over_regset_sections (struct gdbarch *gdbarch,
					iterate_over_regset_sections_cb *cb,
					void *cb_data,
					const struct regcache *regcache)
{
  cb (".reg", C33_LINUX_SIZEOF_GREGSET, C33_LINUX_SIZEOF_GREGSET,
      &c33_linux_gregset, NULL, cb_data);
}

static void
c33_linux_init_abi (struct gdbarch_info info, struct gdbarch *gdbarch)
{
  linux_init_abi (info, gdbarch, 0);
  set_gdbarch_iterate_over_regset_sections
    (gdbarch, c33_linux_iterate_over_regset_sections);
  set_gdbarch_make_solib_ops (gdbarch, make_c33_solib_ops);
}

/* Every program for no-MMU Linux here is FDPIC, and nothing else is.  */

static enum gdb_osabi
c33_linux_elf_osabi_sniffer (bfd *abfd)
{
  if ((elf_elfheader (abfd)->e_flags & EF_C33_FDPIC) != 0)
    return GDB_OSABI_LINUX;
  return GDB_OSABI_UNKNOWN;
}

INIT_GDB_FILE (c33_linux_tdep)
{
  gdbarch_register_osabi (bfd_arch_c33, 0, GDB_OSABI_LINUX,
			  c33_linux_init_abi);
  gdbarch_register_osabi_sniffer (bfd_arch_c33, bfd_target_elf_flavour,
				  c33_linux_elf_osabi_sniffer);
}
