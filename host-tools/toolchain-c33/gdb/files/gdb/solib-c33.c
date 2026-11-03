/* Handle Epson S1C33 FDPIC shared libraries for GDB, the GNU Debugger.

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

/* Without an MMU every module's segments land wherever the loader put
   them, each segment on its own; a load map, as FR-V's FDPIC ABI has,
   says where.  The kernel loads the program and its interpreter and
   reports their maps (PTRACE_GETFDPIC, or a core file's NT_PRSTATUS);
   musl's dynamic linker, which is libc.so, keeps every module's map in
   its link map.  Its debugger interface is _dl_debug_addr, a pointer to

     struct debug { int ver; void *head; void (*bp)(void); int state;
		    void *base; };

   whose head is the first of its modules, each starting

     struct dso { struct fdpic_loadmap *loadmap; char *name; size_t *dynv;
		  struct dso *next, *prev; ... };

   bp is a function pointer, so the address of a descriptor whose first
   word is the entry point GDB breaks at.  This follows solib-frv.c,
   whose link map carries a {map, got} pair where musl's has the map.  */

#include "extract-store-integer.h"
#include "breakpoint.h"
#include "gdb_bfd.h"
#include "gdbcore.h"
#include "inferior.h"
#include "objfiles.h"
#include "solib.h"
#include "symtab.h"
#include "target.h"
#include "c33-tdep.h"
#include "solib-c33.h"

struct c33_solib_ops : public solib_ops
{
  using solib_ops::solib_ops;

  void relocate_section_addresses (solib &so, target_section *) const override;
  void clear_solib (program_space *pspace) const override;
  void create_inferior_hook (int from_tty) const override;
  owning_intrusive_list<solib> current_sos () const override;
  bool in_dynsym_resolve_code (CORE_ADDR pc) const override;
};

solib_ops_up
make_c33_solib_ops (program_space *pspace)
{
  return std::make_unique<c33_solib_ops> (pspace);
}

enum { C33_PTR_SIZE = 4 };

/* A load map, decoded.  */

struct c33_loadseg
{
  CORE_ADDR addr;	/* where the segment is */
  CORE_ADDR p_vaddr;	/* where its program header says */
  CORE_ADDR p_memsz;
};

struct c33_loadmap
{
  std::vector<c33_loadseg> segs;

  /* The displacement for link-time address ADDR, or 0 outside the map.  */
  CORE_ADDR displacement (CORE_ADDR addr) const
  {
    for (const c33_loadseg &seg : segs)
      if (seg.p_vaddr <= addr && addr < seg.p_vaddr + seg.p_memsz)
	return seg.addr - seg.p_vaddr;
    return 0;
  }
};

using c33_loadmap_up = std::unique_ptr<c33_loadmap>;

static bool
same_map (const c33_loadmap &a, const c33_loadmap &b)
{
  if (a.segs.size () != b.segs.size ())
    return false;
  for (size_t i = 0; i < a.segs.size (); i++)
    if (a.segs[i].addr != b.segs[i].addr
	|| a.segs[i].p_vaddr != b.segs[i].p_vaddr)
      return false;
  return true;
}

/* Decode the external form: a 16-bit version, which must be 0, a 16-bit
   count, and that many {addr, p_vaddr, p_memsz} words.  */

static c33_loadmap_up
decode_loadmap (const gdb_byte *buf, size_t len)
{
  bfd_endian order = gdbarch_byte_order (current_inferior ()->arch ());

  if (len < 4 || extract_unsigned_integer (buf, 2, order) != 0)
    return nullptr;
  unsigned int nsegs = extract_unsigned_integer (buf + 2, 2, order);
  if (len < 4 + 12 * (size_t) nsegs)
    return nullptr;

  c33_loadmap_up map (new c33_loadmap);
  for (unsigned int i = 0; i < nsegs; i++)
    {
      const gdb_byte *p = buf + 4 + 12 * i;

      map->segs.push_back ({ extract_unsigned_integer (p, 4, order),
			     extract_unsigned_integer (p + 4, 4, order),
			     extract_unsigned_integer (p + 8, 4, order) });
    }
  return map;
}

static c33_loadmap_up
fetch_loadmap (CORE_ADDR addr)
{
  gdb_byte head[4];

  if (addr == 0 || target_read_memory (addr, head, sizeof head) != 0)
    return nullptr;

  bfd_endian order = gdbarch_byte_order (current_inferior ()->arch ());
  size_t len = 4 + 12 * extract_unsigned_integer (head + 2, 2, order);
  gdb::byte_vector buf (len);

  if (target_read_memory (addr, buf.data (), len) != 0)
    return nullptr;
  return decode_loadmap (buf.data (), len);
}

/* The executable's ("exec") or the interpreter's ("interp") load map,
   and its address: from gdbserver if the target has the maps
   (qXfer:fdpic), whose address is then unknown, else from the
   registers that carry their addresses.  */

static c33_loadmap_up
target_loadmap (const char *which, CORE_ADDR *addrp)
{
  CORE_ADDR interp_addr = 0, exec_addr = 0;

  *addrp = 0;
  std::optional<gdb::byte_vector> buf
    = target_read_alloc (current_inferior ()->top_target (),
			 TARGET_OBJECT_FDPIC, which);
  if (buf && !buf->empty ())
    return decode_loadmap (buf->data (), buf->size ());

  if (c33_fdpic_loadmap_addresses (current_inferior ()->arch (),
				   &interp_addr, &exec_addr) < 0)
    return nullptr;
  *addrp = streq (which, "exec") ? exec_addr : interp_addr;
  return fetch_loadmap (*addrp);
}

struct lm_info_c33 final : public lm_info
{
  lm_info_c33 (c33_loadmap_up map, CORE_ADDR lm_addr)
    : map (std::move (map)), lm_addr (lm_addr)
  {}

  c33_loadmap_up map;
  CORE_ADDR lm_addr;
};

/* What a program space's run of the program has found so far.  */

static bool main_relocated;
static CORE_ADDR main_loadmap_addr;
static c33_loadmap_up main_loadmap;
static CORE_ADDR debug_addr;		/* musl's struct debug */
static bool break_done;
static CORE_ADDR interp_text_low, interp_text_high;

/* Move the main executable's sections to where its load map puts them.  */

static void
relocate_main_executable ()
{
  objfile *objf = current_program_space->symfile_object_file;
  CORE_ADDR addr;

  c33_loadmap_up map = target_loadmap ("exec", &addr);
  if (map == nullptr || objf == nullptr)
    return;

  std::vector<CORE_ADDR> offsets (objf->section_offsets.size ());
  bool changed = false;

  for (obj_section &osect : objf->sections ())
    {
      int idx = &osect - objf->sections_start;
      CORE_ADDR orig = osect.addr () - objf->section_offsets[idx];

      offsets[idx] = objf->section_offsets[idx];
      for (const c33_loadseg &seg : map->segs)
	if (seg.p_vaddr <= orig && orig < seg.p_vaddr + seg.p_memsz)
	  {
	    offsets[idx] = seg.addr - seg.p_vaddr;
	    changed |= offsets[idx] != objf->section_offsets[idx];
	    break;
	  }
    }
  if (changed)
    objfile_relocate (objf, offsets);

  main_loadmap_addr = addr;
  main_loadmap = std::move (map);
  main_relocated = true;
}

/* Find musl's struct debug through _dl_debug_addr in the interpreter,
   set the breakpoint its bp names, and note where the interpreter's code
   is.  Return false until the dynamic linker has filled it in.  */

static bool
find_debug ()
{
  if (debug_addr != 0)
    return true;

  bfd *exec_bfd = current_program_space->exec_bfd ();
  asection *interp_sect = (exec_bfd != nullptr
			   ? bfd_get_section_by_name (exec_bfd, ".interp")
			   : nullptr);
  if (interp_sect == nullptr)
    return false;

  gdb::byte_vector name (bfd_section_size (interp_sect) + 1, 0);
  bfd_get_section_contents (exec_bfd, interp_sect, name.data (), 0,
			    bfd_section_size (interp_sect));

  gdb_bfd_ref_ptr interp;
  try
    {
      interp = solib_bfd_open ((const char *) name.data ());
    }
  catch (const gdb_exception_error &)
    {
    }
  if (interp == nullptr)
    return false;

  CORE_ADDR map_addr;
  c33_loadmap_up map = target_loadmap ("interp", &map_addr);
  if (map == nullptr)
    return false;

  asection *text = bfd_get_section_by_name (interp.get (), ".text");
  if (text != nullptr)
    {
      interp_text_low = bfd_section_vma (text);
      interp_text_low += map->displacement (interp_text_low);
      interp_text_high = interp_text_low + bfd_section_size (text);
    }

  CORE_ADDR sym = gdb_bfd_lookup_symbol (interp.get (),
					 [] (const asymbol *s)
					 {
					   return streq (s->name,
							 "_dl_debug_addr");
					 });
  if (sym == 0)
    {
      warning (_("No _dl_debug_addr in the dynamic linker: GDB cannot "
		 "find its shared libraries."));
      return false;
    }
  sym += map->displacement (sym);

  bfd_endian order = gdbarch_byte_order (current_inferior ()->arch ());
  gdb_byte buf[C33_PTR_SIZE];
  if (target_read_memory (sym, buf, sizeof buf) != 0)
    return false;
  CORE_ADDR addr = extract_unsigned_integer (buf, sizeof buf, order);
  if (addr == 0)
    return false;
  debug_addr = addr;

  /* bp, a descriptor, whose first word is the entry point.  */
  if (!break_done
      && target_read_memory (debug_addr + 8, buf, sizeof buf) == 0)
    {
      CORE_ADDR desc = extract_unsigned_integer (buf, sizeof buf, order);

      if (desc != 0 && target_read_memory (desc, buf, sizeof buf) == 0)
	{
	  remove_solib_event_breakpoints ();
	  create_solib_event_breakpoint
	    (current_inferior ()->arch (),
	     extract_unsigned_integer (buf, sizeof buf, order));
	  break_done = true;
	}
    }
  return true;
}

owning_intrusive_list<solib>
c33_solib_ops::current_sos () const
{
  owning_intrusive_list<solib> sos;
  bfd_endian order = gdbarch_byte_order (current_inferior ()->arch ());

  /* A core file is read before the inferior hook would have run.  */
  if (!main_relocated)
    relocate_main_executable ();
  if (!find_debug ())
    return sos;

  gdb_byte buf[C33_PTR_SIZE];
  if (target_read_memory (debug_addr + 4, buf, sizeof buf) != 0)
    return sos;
  CORE_ADDR lm = extract_unsigned_integer (buf, sizeof buf, order);

  for (int guard = 0; lm != 0 && guard < 1024; guard++)
    {
      gdb_byte dso[5 * C33_PTR_SIZE];

      if (target_read_memory (lm, dso, sizeof dso) != 0)
	{
	  warning (_("Cannot read the dynamic linker's list of modules."));
	  break;
	}
      CORE_ADDR map_addr = extract_unsigned_integer (dso, 4, order);
      CORE_ADDR name_addr = extract_unsigned_integer (dso + 4, 4, order);
      CORE_ADDR next = extract_unsigned_integer (dso + 12, 4, order);

      /* The main program is the executable, not a shared library: the
	 module with its map, by address or, from gdbserver, by contents.  */
      c33_loadmap_up map = fetch_loadmap (map_addr);
      bool is_main = ((map_addr == main_loadmap_addr && map_addr != 0)
		      || (map != nullptr && main_loadmap != nullptr
			  && same_map (*map, *main_loadmap)));
      if (!is_main && name_addr != 0)
	{
	  gdb::unique_xmalloc_ptr<char> name
	    = target_read_string (name_addr, SO_NAME_MAX_PATH_SIZE - 1);

	  if (map != nullptr && name != nullptr && name.get ()[0] != '\0')
	    sos.emplace_back (std::make_unique<lm_info_c33> (std::move (map),
							      lm),
			      name.get (), name.get (), *this);
	}
      lm = next;
    }
  return sos;
}

bool
c33_solib_ops::in_dynsym_resolve_code (CORE_ADDR pc) const
{
  return ((pc >= interp_text_low && pc < interp_text_high)
	  || in_plt_section (pc));
}

void
c33_solib_ops::create_inferior_hook (int from_tty) const
{
  main_relocated = false;
  relocate_main_executable ();

  /* Until the dynamic linker has its list, stop at the program's entry
     point and look again.  */
  if (current_program_space->exec_bfd () == nullptr
      || bfd_get_section_by_name (current_program_space->exec_bfd (),
				  ".interp") == nullptr)
    return;
  std::optional<CORE_ADDR> entry
    = current_program_space->entry_point_address_query ();
  if (entry.has_value () && !find_debug ())
    create_solib_event_breakpoint (current_inferior ()->arch (), *entry);
}

void
c33_solib_ops::clear_solib (program_space *pspace) const
{
  main_relocated = false;
  main_loadmap_addr = 0;
  main_loadmap.reset ();
  debug_addr = 0;
  break_done = false;
  interp_text_low = interp_text_high = 0;
}

void
c33_solib_ops::relocate_section_addresses (solib &so,
					   target_section *sec) const
{
  auto *li = gdb::checked_static_cast<lm_info_c33 *> (so.lm_info.get ());
  CORE_ADDR displ = li->map->displacement (sec->addr);

  sec->addr += displ;
  sec->endaddr += displ;
}
