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

/* The C33 has sixteen general registers, a stack pointer of its own,
   the multiply-accumulate pair alr/ahr, the flags in psr, and a PC.  A
   call pushes the return address; there is no link register.  The
   compiler describes frames in DWARF, with the return address in a
   column of its own one past its registers, which is the PC here.  */

#include "extract-store-integer.h"
#include "arch-utils.h"
#include "dis-asm.h"
#include "dwarf2/frame.h"
#include "frame.h"
#include "frame-base.h"
#include "frame-unwind.h"
#include "gdbcore.h"
#include "gdbtypes.h"
#include "inferior.h"
#include "objfiles.h"
#include "osabi.h"
#include "regcache.h"
#include "symtab.h"
#include "target.h"
#include "trad-frame.h"
#include "value.h"

#include "elf-bfd.h"
#include "elf/c33.h"
#include "c33-tdep.h"

struct c33_gdbarch_tdep : gdbarch_tdep_base
{
  /* Whether the program is FDPIC, and so has load maps.  */
  bool fdpic = false;
};

static const char *const c33_register_names[] =
{
  "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
  "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
  "sp", "alr", "ahr", "psr", "pc",
  "loadmap_exec", "loadmap_interp"
};

static const char *
c33_register_name (struct gdbarch *gdbarch, int regnum)
{
  static_assert (ARRAY_SIZE (c33_register_names) == C33_NUM_REGS);
  c33_gdbarch_tdep *tdep = gdbarch_tdep<c33_gdbarch_tdep> (gdbarch);

  /* The load maps are a property of an FDPIC program only.  */
  if (!tdep->fdpic && regnum >= C33_LOADMAP_EXEC_REGNUM)
    return "";
  return c33_register_names[regnum];
}

static struct type *
c33_register_type (struct gdbarch *gdbarch, int regnum)
{
  switch (regnum)
    {
    case C33_PC_REGNUM:
      return builtin_type (gdbarch)->builtin_func_ptr;
    case C33_SP_REGNUM:
    case C33_LOADMAP_EXEC_REGNUM:
    case C33_LOADMAP_INTERP_REGNUM:
      return builtin_type (gdbarch)->builtin_data_ptr;
    default:
      return builtin_type (gdbarch)->builtin_uint32;
    }
}

/* GCC's DWARF numbers are its register numbers: r0-r15, sp, alr, ahr and
   its condition code, which is psr.  */

static int
c33_dwarf2_reg_to_regnum (struct gdbarch *gdbarch, int reg)
{
  if (reg >= 0 && reg <= C33_AHR_REGNUM)
    return reg;
  if (reg == 19)
    return C33_PSR_REGNUM;
  if (reg == C33_DWARF_RETURN_COLUMN)
    return C33_PC_REGNUM;
  return -1;
}

static void
c33_dwarf2_frame_init_reg (struct gdbarch *gdbarch, int regnum,
			   struct dwarf2_frame_state_reg *reg,
			   const frame_info_ptr &this_frame)
{
  if (regnum == C33_SP_REGNUM)
    reg->how = DWARF2_FRAME_REG_CFA;
  else if (regnum == C33_PC_REGNUM)
    reg->how = DWARF2_FRAME_REG_RA;
}

constexpr gdb_byte c33_break_insn[] = { C33_BREAKPOINT_INSN & 0xff,
					C33_BREAKPOINT_INSN >> 8 };

using c33_breakpoint = BP_MANIPULATION (c33_break_insn);

/* The stack stays 16-byte aligned.  */

static CORE_ADDR
c33_frame_align (struct gdbarch *gdbarch, CORE_ADDR sp)
{
  return sp & ~(CORE_ADDR) 15;
}

/* The prologue ends where the line table says the body starts.  */

static CORE_ADDR
c33_skip_prologue (struct gdbarch *gdbarch, CORE_ADDR pc)
{
  CORE_ADDR func_addr;

  if (find_pc_partial_function (pc, NULL, &func_addr, NULL))
    {
      CORE_ADDR post = skip_prologue_using_sal (gdbarch, func_addr);

      if (post != 0)
	return std::max (pc, post);
    }
  return pc;
}

/* Values of up to eight bytes with a scalar mode come back in r4 and r5;
   anything else is written through a pointer the caller passes in r6,
   which the callee returns in r4.  An aggregate has a scalar mode when it
   is one, two, four or eight bytes.  */

static bool
c33_in_memory (struct type *type)
{
  type = check_typedef (type);
  ULONGEST len = type->length ();

  if (len > 8)
    return true;
  switch (type->code ())
    {
    case TYPE_CODE_STRUCT:
    case TYPE_CODE_UNION:
    case TYPE_CODE_ARRAY:
      return len != 1 && len != 2 && len != 4 && len != 8;
    default:
      return false;
    }
}

static enum return_value_convention
c33_return_value (struct gdbarch *gdbarch, struct value *function,
		  struct type *type, struct regcache *regcache,
		  struct value **read_value, const gdb_byte *writebuf)
{
  int len = check_typedef (type)->length ();

  if (c33_in_memory (type))
    {
      if (read_value != NULL)
	{
	  ULONGEST addr;

	  regcache_cooked_read_unsigned (regcache, C33_R4_REGNUM, &addr);
	  *read_value = value_at_non_lval (type, addr);
	}
      return RETURN_VALUE_ABI_RETURNS_ADDRESS;
    }

  if (read_value != NULL)
    {
      *read_value = value::allocate (type);
      gdb_byte *buf = (*read_value)->contents_raw ().data ();
      gdb_byte regs[8];

      regcache->cooked_read (C33_R4_REGNUM, regs);
      if (len > 4)
	regcache->cooked_read (C33_R5_REGNUM, regs + 4);
      memcpy (buf, regs, len);
    }
  if (writebuf != NULL)
    {
      gdb_byte regs[8] = { 0 };

      memcpy (regs, writebuf, len);
      regcache->cooked_write (C33_R4_REGNUM, regs);
      if (len > 4)
	regcache->cooked_write (C33_R5_REGNUM, regs + 4);
    }
  return RETURN_VALUE_REGISTER_CONVENTION;
}

/* Whether ADDR is in a code section GDB knows.  */

static bool
c33_in_code (CORE_ADDR addr)
{
  struct obj_section *sect = find_pc_section (addr);

  return sect != NULL && (sect->the_bfd_section->flags & SEC_CODE) != 0;
}

/* An FDPIC function pointer is the address of a descriptor: the entry
   point, then the function's data segment pointer.  An address in code
   is an entry point already, and so is anything that is not a readable
   descriptor of one: GDB also passes ELF entry points here, before their
   module is placed.  */

static CORE_ADDR
c33_convert_from_func_ptr_addr (struct gdbarch *gdbarch, CORE_ADDR addr,
				struct target_ops *targ)
{
  c33_gdbarch_tdep *tdep = gdbarch_tdep<c33_gdbarch_tdep> (gdbarch);
  gdb_byte buf[4];

  if (!tdep->fdpic || c33_in_code (addr)
      || target_read (targ, TARGET_OBJECT_MEMORY, NULL, buf, addr, 4) != 4)
    return addr;

  CORE_ADDR entry = extract_unsigned_integer (buf, 4,
					      gdbarch_byte_order (gdbarch));
  return c33_in_code (entry) ? entry : addr;
}

int
c33_fdpic_loadmap_addresses (struct gdbarch *gdbarch, CORE_ADDR *interp_addr,
			     CORE_ADDR *exec_addr)
{
  c33_gdbarch_tdep *tdep = gdbarch_tdep<c33_gdbarch_tdep> (gdbarch);

  if (!tdep->fdpic)
    return -1;

  regcache *regcache = get_thread_regcache (inferior_thread ());
  ULONGEST val;

  if (interp_addr != NULL)
    {
      regcache_cooked_read_unsigned (regcache, C33_LOADMAP_INTERP_REGNUM,
				     &val);
      *interp_addr = val;
    }
  if (exec_addr != NULL)
    {
      regcache_cooked_read_unsigned (regcache, C33_LOADMAP_EXEC_REGNUM,
				     &val);
      *exec_addr = val;
    }
  return 0;
}

/* Software single step.  There is no hardware step for user programs,
   so GDB puts breakpoints where the instruction at PC can go next.  An
   ext prefix and the instruction it extends, and a delayed branch and the
   instruction in its slot, run as one: the hardware takes no interrupt
   between them, and nothing may stop there.  Every instruction is two
   bytes.  The encodings are the PE core's (class 0):

     bits 15-8 = 0x08-0x1f, low byte sign8: jr<cc> (0x08-0x1b, ten
       conditions), call (0x1c) and jp (0x1e), odd for the delayed forms;
       the target is the branch's own address plus twice the displacement,
       which ext prefixes widen;
     bits 15-6, rb in bits 3-0: call %rb 0x18, ret 0x19, jp %rb 0x1a,
       jpr %rb 0x0b (PC-relative), and delayed 0x1c, 0x1d, 0x1e, 0x0f.  */

static bool
c33_is_ext (unsigned int insn)
{
  return (insn >> 13) == 6;
}

static std::vector<CORE_ADDR>
c33_software_single_step (struct regcache *regcache)
{
  struct gdbarch *gdbarch = regcache->arch ();
  enum bfd_endian order = gdbarch_byte_order_for_code (gdbarch);
  CORE_ADDR pc = regcache_read_pc (regcache);
  unsigned int ext[2];
  int n_ext = 0;
  unsigned int insn;
  CORE_ADDR at = pc;

  for (;;)
    {
      insn = read_memory_unsigned_integer (at, 2, order);
      if (!c33_is_ext (insn) || n_ext == 2)
	break;
      ext[n_ext++] = insn & 0x1fff;
      at += 2;
    }

  unsigned int op8 = insn >> 8, op10 = insn >> 6;
  CORE_ADDR next = at + 2;

  if (op8 >= 0x08 && op8 <= 0x1f)
    {
      /* The halfword displacement, sign-extended from its composed
	 width: 8 bits alone, 21 with one ext, 31 with two (of the first
	 prefix only bits 12-3 take part).  */
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
      bool delayed = (op8 & 1) != 0;
      bool conditional = op8 < 0x1c;

      if (delayed)
	next = at + 4;
      if (!conditional)
	return { target };
      return { next, target };
    }

  if ((insn >> 13) == 0)
    {
      ULONGEST rb, sp, ra;

      regcache_cooked_read_unsigned (regcache, C33_R0_REGNUM + (insn & 0xf),
				     &rb);
      switch (op10)
	{
	case 0x18: case 0x1c:	/* call %rb */
	case 0x1a: case 0x1e:	/* jp %rb */
	  return { rb & ~(CORE_ADDR) 1 };
	case 0x0b: case 0x0f:	/* jpr %rb */
	  return { (at + (rb & ~(ULONGEST) 1)) & 0xffffffff };
	case 0x19: case 0x1d:	/* ret */
	  regcache_cooked_read_unsigned (regcache, C33_SP_REGNUM, &sp);
	  ra = read_memory_unsigned_integer (sp, 4, order);
	  return { ra & ~(CORE_ADDR) 1 };
	}
    }

  return { next };
}

/* The fallback, for code without call frame information: a function
   that has not yet moved the stack, its return address at [sp].  Leaf
   code written in assembly, and the first instruction of anything, are
   like this.  */

struct c33_frame_cache
{
  CORE_ADDR cfa;
  CORE_ADDR func;
};

static struct c33_frame_cache *
c33_frame_cache (const frame_info_ptr &this_frame, void **this_cache)
{
  if (*this_cache != NULL)
    return (struct c33_frame_cache *) *this_cache;

  auto *cache = frame_obstack_zalloc<struct c33_frame_cache> ();
  *this_cache = cache;
  cache->cfa = get_frame_register_unsigned (this_frame, C33_SP_REGNUM) + 4;
  cache->func = get_frame_func (this_frame);
  return cache;
}

static void
c33_frame_this_id (const frame_info_ptr &this_frame, void **this_cache,
		   struct frame_id *this_id)
{
  struct c33_frame_cache *cache = c33_frame_cache (this_frame, this_cache);

  if (cache->func == 0)
    return;
  *this_id = frame_id_build (cache->cfa, cache->func);
}

static struct value *
c33_frame_prev_register (const frame_info_ptr &this_frame, void **this_cache,
			 int regnum)
{
  struct c33_frame_cache *cache = c33_frame_cache (this_frame, this_cache);

  if (regnum == C33_SP_REGNUM)
    return frame_unwind_got_constant (this_frame, regnum, cache->cfa);
  if (regnum == C33_PC_REGNUM)
    return frame_unwind_got_memory (this_frame, regnum, cache->cfa - 4);
  return frame_unwind_got_register (this_frame, regnum, regnum);
}

static const struct frame_unwind_legacy c33_frame_unwind (
  "c33 no frame",
  NORMAL_FRAME,
  FRAME_UNWIND_ARCH,
  default_frame_unwind_stop_reason,
  c33_frame_this_id,
  c33_frame_prev_register,
  NULL,
  default_frame_sniffer
);

static struct gdbarch *
c33_gdbarch_init (struct gdbarch_info info, struct gdbarch_list *arches)
{
  bool fdpic = (info.abfd != NULL
		&& bfd_get_flavour (info.abfd) == bfd_target_elf_flavour
		&& (elf_elfheader (info.abfd)->e_flags & EF_C33_FDPIC) != 0);

  for (arches = gdbarch_list_lookup_by_info (arches, &info);
       arches != NULL;
       arches = gdbarch_list_lookup_by_info (arches->next, &info))
    {
      c33_gdbarch_tdep *tdep
	= gdbarch_tdep<c33_gdbarch_tdep> (arches->gdbarch);

      if (tdep->fdpic == fdpic)
	return arches->gdbarch;
    }

  gdbarch *gdbarch
    = gdbarch_alloc (&info, gdbarch_tdep_up (new c33_gdbarch_tdep));
  c33_gdbarch_tdep *tdep = gdbarch_tdep<c33_gdbarch_tdep> (gdbarch);
  tdep->fdpic = fdpic;

  set_gdbarch_num_regs (gdbarch, C33_NUM_REGS);
  set_gdbarch_sp_regnum (gdbarch, C33_SP_REGNUM);
  set_gdbarch_pc_regnum (gdbarch, C33_PC_REGNUM);
  set_gdbarch_ps_regnum (gdbarch, C33_PSR_REGNUM);
  set_gdbarch_register_name (gdbarch, c33_register_name);
  set_gdbarch_register_type (gdbarch, c33_register_type);
  set_gdbarch_dwarf2_reg_to_regnum (gdbarch, c33_dwarf2_reg_to_regnum);

  set_gdbarch_char_signed (gdbarch, 1);
  set_gdbarch_short_bit (gdbarch, 16);
  set_gdbarch_int_bit (gdbarch, 32);
  set_gdbarch_long_bit (gdbarch, 32);
  set_gdbarch_long_long_bit (gdbarch, 64);
  set_gdbarch_ptr_bit (gdbarch, 32);
  set_gdbarch_float_bit (gdbarch, 32);
  set_gdbarch_double_bit (gdbarch, 64);
  set_gdbarch_long_double_bit (gdbarch, 64);
  set_gdbarch_long_double_format (gdbarch, floatformats_ieee_double);
  set_gdbarch_wchar_bit (gdbarch, 32);
  set_gdbarch_wchar_signed (gdbarch, 1);

  set_gdbarch_inner_than (gdbarch, core_addr_lessthan);
  set_gdbarch_frame_align (gdbarch, c33_frame_align);
  set_gdbarch_skip_prologue (gdbarch, c33_skip_prologue);
  set_gdbarch_return_value_as_value (gdbarch, c33_return_value);
  set_gdbarch_breakpoint_kind_from_pc (gdbarch, c33_breakpoint::kind_from_pc);
  set_gdbarch_sw_breakpoint_from_kind (gdbarch, c33_breakpoint::bp_from_kind);
  set_gdbarch_decr_pc_after_break (gdbarch, 0);
  set_gdbarch_get_next_pcs (gdbarch, c33_software_single_step);
  set_gdbarch_convert_from_func_ptr_addr (gdbarch,
					  c33_convert_from_func_ptr_addr);

  dwarf2_frame_set_init_reg (gdbarch, c33_dwarf2_frame_init_reg);

  /* The OS ABI's unwinders come first, then DWARF, then the fallback.  */
  gdbarch_init_osabi (info, gdbarch);
  dwarf2_append_unwinders (gdbarch);
  frame_base_append_sniffer (gdbarch, dwarf2_frame_base_sniffer);
  frame_unwind_append_unwinder (gdbarch, &c33_frame_unwind);

  return gdbarch;
}

INIT_GDB_FILE (c33_tdep)
{
  gdbarch_register (bfd_arch_c33, c33_gdbarch_init);
}
