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
#include "frame.h"
#include "linux-tdep.h"
#include "symtab.h"
#include "osabi.h"
#include "regcache.h"
#include "regset.h"
#include "trad-frame.h"
#include "tramp-frame.h"
#include "c33-tdep.h"
#include "solib-c33.h"

/* The kernel's struct pt_regs, which is NT_PRSTATUS's pr_reg, and after
   it the two load map addresses of struct elf_prstatus_fdpic, which BFD
   includes in a core file's .reg, as FR-V's does.  */

#define C33_LINUX_PT_REGS_MAP						\
    { 16, C33_R0_REGNUM, 4 },						\
    { 1, C33_ALR_REGNUM, 4 },						\
    { 1, C33_AHR_REGNUM, 4 },						\
    { 1, C33_SP_REGNUM, 4 },						\
    { 1, REGCACHE_MAP_SKIP, 4 },	/* vector */			\
    { 1, REGCACHE_MAP_SKIP, 4 },	/* orig_r4 */			\
    { 1, REGCACHE_MAP_SKIP, 4 },	/* reserved */			\
    { 1, C33_PSR_REGNUM, 4 },						\
    { 1, C33_PC_REGNUM, 4 }

static const struct regcache_map_entry c33_linux_gregmap[] =
  {
    C33_LINUX_PT_REGS_MAP,
    { 0 }
  };

static const struct regcache_map_entry c33_linux_fdpic_gregmap[] =
  {
    C33_LINUX_PT_REGS_MAP,
    { 1, C33_LOADMAP_EXEC_REGNUM, 4 },
    { 1, C33_LOADMAP_INTERP_REGNUM, 4 },
    { 0 }
  };

#define C33_LINUX_SIZEOF_PT_REGS (24 * 4)
#define C33_LINUX_SIZEOF_FDPIC_GREGSET (C33_LINUX_SIZEOF_PT_REGS + 2 * 4)

static const struct regset c33_linux_gregset =
  {
    c33_linux_gregmap, regcache_supply_regset, regcache_collect_regset
  };

static const struct regset c33_linux_fdpic_gregset =
  {
    c33_linux_fdpic_gregmap, regcache_supply_regset, regcache_collect_regset
  };

static void
c33_linux_iterate_over_regset_sections (struct gdbarch *gdbarch,
					iterate_over_regset_sections_cb *cb,
					void *cb_data,
					const struct regcache *regcache)
{
  if (gdbarch_tdep<c33_gdbarch_tdep> (gdbarch)->loadmap_regs)
    cb (".reg", C33_LINUX_SIZEOF_FDPIC_GREGSET,
	C33_LINUX_SIZEOF_FDPIC_GREGSET, &c33_linux_fdpic_gregset, NULL,
	cb_data);
  else
    cb (".reg", C33_LINUX_SIZEOF_PT_REGS, C33_LINUX_SIZEOF_PT_REGS,
	&c33_linux_gregset, NULL, cb_data);
}

/* A core file's .reg has the load map addresses.  */

static const struct target_desc *
c33_linux_core_read_description (struct gdbarch *gdbarch,
				 struct target_ops *target, bfd *abfd)
{
  c33_features features;

  features.fdpic_loadmaps = true;
  return c33_lookup_target_description (features);
}

/* A signal handler returns to the kernel's trampoline, which calls
   rt_sigreturn:

     ext 2; ld.w %r4,0xb	xld.w %r4,139 (__NR_rt_sigreturn)
     int 0

   The handler's return popped the frame's return address, so the stack
   pointer is at the frame's struct siginfo (128 bytes), then struct
   ucontext: uc_flags, uc_link, uc_stack (12 bytes), and uc_mcontext,
   the interrupted r0-r15, alr, ahr, sp, psr and pc.  */

#define C33_SIGINFO_SIZE 128
#define C33_UC_MCONTEXT 20

static void
c33_linux_rt_sigframe_init (const struct tramp_frame *self,
			    const frame_info_ptr &this_frame,
			    struct trad_frame_cache *this_cache,
			    CORE_ADDR func)
{
  CORE_ADDR sp = get_frame_register_unsigned (this_frame, C33_SP_REGNUM);
  CORE_ADDR sc = sp + C33_SIGINFO_SIZE + C33_UC_MCONTEXT;

  for (int i = 0; i < 16; i++)
    trad_frame_set_reg_addr (this_cache, C33_R0_REGNUM + i, sc + 4 * i);
  trad_frame_set_reg_addr (this_cache, C33_ALR_REGNUM, sc + 64);
  trad_frame_set_reg_addr (this_cache, C33_AHR_REGNUM, sc + 68);
  trad_frame_set_reg_addr (this_cache, C33_SP_REGNUM, sc + 72);
  trad_frame_set_reg_addr (this_cache, C33_PSR_REGNUM, sc + 76);
  trad_frame_set_reg_addr (this_cache, C33_PC_REGNUM, sc + 80);
  trad_frame_set_id (this_cache, frame_id_build (sp, func));
}

static const struct tramp_frame c33_linux_rt_sigframe =
{
  SIGTRAMP_FRAME,
  2,
  {
    { 0xc002, ULONGEST_MAX },
    { 0x6cb4, ULONGEST_MAX },
    { 0x0480, ULONGEST_MAX },
    { TRAMP_SENTINEL_INSN, ULONGEST_MAX }
  },
  c33_linux_rt_sigframe_init
};

static void
c33_linux_init_abi (struct gdbarch_info info, struct gdbarch *gdbarch)
{
  linux_init_abi (info, gdbarch, 0);
  tramp_frame_prepend_unwinder (gdbarch, &c33_linux_rt_sigframe);
  /* Step through FUNCTION@plt to FUNCTION.  */
  set_gdbarch_skip_trampoline_code (gdbarch, find_solib_trampoline_target);
  set_gdbarch_skip_solib_resolver (gdbarch, c33_skip_solib_resolver);
  set_gdbarch_iterate_over_regset_sections
    (gdbarch, c33_linux_iterate_over_regset_sections);
  set_gdbarch_core_read_description (gdbarch,
				     c33_linux_core_read_description);
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
