/* c33-specific support for 32-bit ELF
   Copyright (C) 1996, 1997, 1998, 1999,2001 Free Software Foundation, Inc.

This file is part of BFD, the Binary File Descriptor library.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.  */



/* XXX FIXME: This code is littered with 32bit int, 16bit short, 8bit char
   dependencies.  As is the gas & simulator code or the c33.  */


#include "sysdep.h"
#include "bfd.h"
#include "bfdlink.h"
#include "libbfd.h"
#include "elf-bfd.h"
#include "elf/c33.h"
#include "libiberty.h"		// ADDED D.Fujimoto 2007/10/02

/* sign-extend a 24-bit number */
#define SEXT24(x)	((((x) & 0xffffff) ^ (~ 0x7fffff)) + 0x800000)
      
static reloc_howto_type *c33_elf_reloc_type_lookup
  (bfd *abfd, bfd_reloc_code_real_type code);
static bool c33_elf_info_to_howto_rel
  (bfd *, arelent *, Elf_Internal_Rela *);
static bool c33_elf_info_to_howto_rela
  (bfd *, arelent *, Elf_Internal_Rela *);
static bfd_reloc_status_type c33_elf_reloc
  (bfd *, arelent *, asymbol *, void *, asection *, bfd *, char **);
static bool c33_elf_is_local_label_name
  (bfd *, const char *);
static int c33_elf_relocate_section
  (struct bfd_link_info *, bfd *, asection *, bfd_byte *,
	  Elf_Internal_Rela *, Elf_Internal_Sym *, asection **);
static bfd_reloc_status_type c33_elf_perform_relocation
  (bfd *, int, bfd_vma, bfd_byte *,unsigned long);
static bool c33_elf_check_relocs
  (bfd *, struct bfd_link_info *, asection *, const Elf_Internal_Rela *);

#if 0
static void remember_ah_reloc
  (bfd *, bfd_vma, bfd_byte *);
static bfd_byte * find_remembered_ah_reloc
  (bfd_vma,bfd_byte*);
#endif

static bfd_reloc_status_type c33_elf_final_link_relocate
  (reloc_howto_type *, bfd *, bfd *, asection *, bfd_byte *, bfd_vma,
	   bfd_vma, bfd_vma, struct bfd_link_info *, asection *, int);
#if 0
static bool c33_elf_object_p
  (bfd *);
#endif
static bool c33_elf_fake_sections
  (bfd *, Elf_Internal_Shdr *, asection *);
#if 0
static void c33_elf_final_write_processing
  (bfd *, bool);
#endif
static bool c33_elf_set_private_flags
  (bfd *, flagword);
static bool c33_elf_copy_private_bfd_data
  (bfd *, bfd *);
static const char* c33_elf_get_mode_string
  (char mode_flag);
static bool c33_elf_merge_private_bfd_data
  (bfd *, struct bfd_link_info *);
static bool c33_elf_print_private_bfd_data
  (bfd *, void *);
static bool c33_elf_section_from_bfd_section
  (bfd *, asection *, int *);
static void c33_elf_symbol_processing
  (bfd *, asymbol *);
static bool c33_elf_add_symbol_hook
  (bfd *, struct bfd_link_info *, Elf_Internal_Sym *,
	   const char **, flagword *, asection **, bfd_vma *);
static int c33_elf_link_output_symbol_hook
  (struct bfd_link_info *, const char *, Elf_Internal_Sym *,
	   asection *, struct elf_link_hash_entry *);
static bool c33_elf_section_from_shdr
  (bfd *, Elf_Internal_Shdr *, const char *, int);
static bool c33_elf_sym_is_global
  (bfd *, asymbol *);

/* C33 represents its data-area common classes as real sections carrying
   SEC_IS_COMMON.  A section symbol is always local in ELF, even when its
   section has common semantics.  The generic classifier treats every symbol
   in a common section as global; that puts the section symbol after sh_info
   and creates an invalid symbol table.  Keep the generic common-symbol rule
   for ordinary symbols, but exempt section symbols.  */
static bool
c33_elf_sym_is_global (bfd *abfd ATTRIBUTE_UNUSED, asymbol *sym)
{
  if ((sym->flags & BSF_SECTION_SYM) != 0)
    return false;

  return ((sym->flags & (BSF_GLOBAL | BSF_WEAK | BSF_GNU_UNIQUE)) != 0
          || bfd_is_und_section (bfd_asymbol_section (sym))
          || bfd_is_com_section (bfd_asymbol_section (sym)));
}

/* �V���{���̃����P�[�V������� */
/* Note: It is REQUIRED that the 'type' value of each entry in this array
   match the index of the entry in the array.  */
static reloc_howto_type c33_elf_howto_table[] =
{
  /* This reloc does nothing.  */
  HOWTO (R_C33_NONE,			/* type */
	 0,				/* rightshift */
	 4,				/* size (in bytes) */
	 32,				/* bitsize */
	 false,				/* pc_relative */
	 0,				/* bitpos */
	 complain_overflow_bitfield,	/* complain_on_overflow */
	 bfd_elf_generic_reloc,		/* special_function */
	 "R_C33_NONE",			/* name */
	 false,				/* partial_inplace */
	 0,				/* src_mask */
	 0,				/* dst_mask */
	 false),			/* pcrel_offset */

  /* Simple 32bit reloc.  */
  HOWTO (R_C33_32,			/* type */
	 0,				/* rightshift */
	 4,				/* size (in bytes) */
	 32,				/* bitsize */
	 false,				/* pc_relative */
	 0,				/* bitpos */
	 complain_overflow_dont,	/* complain_on_overflow */
	 bfd_elf_generic_reloc,		/* special_function */
	 "R_C33_32",			/* name */
	 false,				/* partial_inplace */
	 0xffffffff,			/* src_mask */
	 0xffffffff,			/* dst_mask */
	 false),			/* pcrel_offset */

  /* Simple 16bit reloc.  */
  HOWTO (R_C33_16,			/* type */
	 0,				/* rightshift */
	 2,				/* size (in bytes) */
	 16,				/* bitsize */
	 false,				/* pc_relative */
	 0,				/* bitpos */
	 complain_overflow_dont,	/* complain_on_overflow */
	 bfd_elf_generic_reloc,		/* special_function */
	 "R_C33_16",			/* name */
	 false,				/* partial_inplace */
	 0xffff,			/* src_mask */
	 0xffff,			/* dst_mask */
	 false),			/* pcrel_offset */

  /* Simple 8bit reloc.	 */
  HOWTO (R_C33_8,			/* type */
	 0,				/* rightshift */
	 1,				/* size (in bytes) */
	 8,				/* bitsize */
	 false,				/* pc_relative */
	 0,				/* bitpos */
	 complain_overflow_dont,	/* complain_on_overflow */
	 bfd_elf_generic_reloc,		/* special_function */
	 "R_C33_8",			/* name */
	 false,				/* partial_inplace */
	 0xff,				/* src_mask */
	 0xff,				/* dst_mask */
	 false),			/* pcrel_offset */

  /* @ah LABEL(25:13) */
  HOWTO (R_C33_AH,	/* type */
	 0,			/* rightshift */
	 2,				/* size (in bytes) */
	 13,				/* bitsize */
	 false,				/* pc_relative */
	 0,				/* bitpos */
	 complain_overflow_dont,	/* complain_on_overflow */
	 c33_elf_reloc,		/* special_function */
	 "R_C33_AH",	/* name */
	 false,				/* partial_inplace */
	 0xffff,			/* src_mask */
	 0xffff,			/* dst_mask */
	 false),			/* pcrel_offset */

  /* @al LABEL(12:0) */
  HOWTO (R_C33_AL,	/* type */
	 0,				/* rightshift */
	 2,				/* size (in bytes) */
	 13,				/* bitsize */
	 false,				/* pc_relative */
	 0,				/* bitpos */
	 complain_overflow_dont,	/* complain_on_overflow */
	 c33_elf_reloc,		/* special_function */
	 "R_C33_AL",	/* name */
	 false,				/* partial_inplace */
	 0xffff,			/* src_mask */
	 0xffff,			/* dst_mask */
	 false),			/* pcrel_offset */

  /* @rh <LABEL-PC>(31:22) */
  HOWTO (R_C33_RH,	/* type */
	 0,				/* rightshift */
	 2,				/* size (in bytes) */
	 13,				/* bitsize */
	 true,				/* pc_relative */
	 0,				/* bitpos */
	 complain_overflow_dont,	/* complain_on_overflow */
	 c33_elf_reloc,		/* special_function */
	 "R_C33_RH",	/* name */
	 false,				/* partial_inplace */
	 0xffff,			/* src_mask */
	 0xffff,			/* dst_mask */
	 false),			/* pcrel_offset */

  /* @rm <LABEL-PC>(21:9) */
  HOWTO (R_C33_RM,	/* type */
	 0,				/* rightshift */
	 2,				/* size (in bytes) */
	 13,				/* bitsize */
	 true,				/* pc_relative */
	 0,				/* bitpos */
	 complain_overflow_dont,	/* complain_on_overflow */
	 c33_elf_reloc,		/* special_function */
	 "R_C33_RM",	/* name */
	 false,				/* partial_inplace */
	 0xffff,			/* src_mask */
	 0xffff,			/* dst_mask */
	 false),			/* pcrel_offset */

  /* @rl <LABEL-PC>(8:0) */
  HOWTO (R_C33_RL,	/* type */
	 0,				/* rightshift */
	 2,				/* size (in bytes) */
	 13,				/* bitsize */
	 true,				/* pc_relative */
	 0,				/* bitpos */
	 complain_overflow_dont,	/* complain_on_overflow */
	 c33_elf_reloc,		/* special_function */
	 "R_C33_RL",	/* name */
	 false,				/* partial_inplace */
	 0xffff,			/* src_mask */
	 0xffff,			/* dst_mask */
	 false),			/* pcrel_offset */

  /* @h LABEL(31:19) */
  HOWTO (R_C33_H,	/* type */
	 0,				/* rightshift */
	 2,				/* size (in bytes) */
	 13,				/* bitsize */
	 false,				/* pc_relative */
	 0,				/* bitpos */
	 complain_overflow_dont,	/* complain_on_overflow */
	 c33_elf_reloc,		/* special_function */
	 "R_C33_H",	/* name */
	 false,				/* partial_inplace */
	 0xffff,			/* src_mask */
	 0xffff,			/* dst_mask */
	 false),			/* pcrel_offset */

  /* @m LABEL(18:6) */
  HOWTO (R_C33_M,	/* type */
	 0,				/* rightshift */
	 2,				/* size (in bytes) */
	 13,				/* bitsize */
	 false,				/* pc_relative */
	 0,				/* bitpos */
	 complain_overflow_dont,	/* complain_on_overflow */
	 c33_elf_reloc,		/* special_function */
	 "R_C33_M",	/* name */
	 false,				/* partial_inplace */
	 0xffff,			/* src_mask */
	 0xffff,			/* dst_mask */
	 false),			/* pcrel_offset */

  /* @l LABEL(5:0) */
  HOWTO (R_C33_L,		/* type */
	 0,					/* rightshift */
	 2,					/* size (in bytes) */
	 6,					/* bitsize */
	 false,				/* pc_relative */
	 0,					/* bitpos */
	 complain_overflow_dont,	/* complain_on_overflow */
	 c33_elf_reloc,		/* special_function */
	 "R_C33_L",			/* name */
	 false,				/* partial_inplace */
	 0xffff,			/* src_mask */
	 0xffff,			/* dst_mask */
	 false),			/* pcrel_offset */

  /* doff_hi(LABEL(25:13)) */  /* add tazaki 2002.01.11 */
  HOWTO (R_C33_DH,		/* type */
	 0,					/* rightshift */
	 2,					/* size (in bytes) */
	 13,				/* bitsize */
	 false,				/* pc_relative */
	 0,					/* bitpos */
	 complain_overflow_dont,	/* complain_on_overflow */
	 c33_elf_reloc,		/* special_function */
	 "R_C33_DH",		/* name */
	 false,				/* partial_inplace */
	 0xffff,			/* src_mask */
	 0xffff,			/* dst_mask */
	 false),			/* pcrel_offset */

  /* doff_lo(LABEL(12:0)) */  /* add tazaki 2002.01.11 */
  HOWTO (R_C33_DL,		/* type */
	 0,					/* rightshift */
	 2,					/* size (in bytes) */
	 13,				/* bitsize */
	 false,				/* pc_relative */
	 0,					/* bitpos */
	 complain_overflow_dont,	/* complain_on_overflow */
	 c33_elf_reloc,		/* special_function */
	 "R_C33_DL",		/* name */
	 false,				/* partial_inplace */
	 0xffff,			/* src_mask */
	 0xffff,			/* dst_mask */
	 false),			/* pcrel_offset */

  /* goff_lo(LABEL(12:0)) */  /* add tazaki 2001.07.24 */
  HOWTO (R_C33_GL,		/* type */
	 0,					/* rightshift */
	 2,					/* size (in bytes) */
	 13,				/* bitsize */
	 false,				/* pc_relative */
	 0,					/* bitpos */
	 complain_overflow_dont,	/* complain_on_overflow */
	 c33_elf_reloc,		/* special_function */
	 "R_C33_GL",		/* name */
	 false,				/* partial_inplace */
	 0xffff,			/* src_mask */
	 0xffff,			/* dst_mask */
	 false),			/* pcrel_offset */

  /* soff_hi(LABEL(25:13)) */  /* add tazaki 2001.11.07 */
  HOWTO (R_C33_SH,	/* type */
	 0,					/* rightshift */
	 2,					/* size (in bytes) */
	 13,				/* bitsize */
	 false,				/* pc_relative */
	 0,					/* bitpos */
	 complain_overflow_dont,	/* complain_on_overflow */
	 c33_elf_reloc,		/* special_function */
	 "R_C33_SH",		/* name */
	 false,				/* partial_inplace */
	 0xffff,			/* src_mask */
	 0xffff,			/* dst_mask */
	 false),			/* pcrel_offset */

  /* soff_lo(LABEL(12:0)) */  /* add tazaki 2001.07.24 */
  HOWTO (R_C33_SL,		/* type */
	 0,					/* rightshift */
	 2,					/* size (in bytes) */
	 13,				/* bitsize */
	 false,				/* pc_relative */
	 0,					/* bitpos */
	 complain_overflow_dont,	/* complain_on_overflow */
	 c33_elf_reloc,		/* special_function */
	 "R_C33_SL",		/* name */
	 false,				/* partial_inplace */
	 0xffff,			/* src_mask */
	 0xffff,			/* dst_mask */
	 false),			/* pcrel_offset */

  /* toff_hi(LABEL(25:13)) */  /* add tazaki 2001.11.07 */
  HOWTO (R_C33_TH,	/* type */
	 0,					/* rightshift */
	 2,					/* size (in bytes) */
	 13,				/* bitsize */
	 false,				/* pc_relative */
	 0,					/* bitpos */
	 complain_overflow_dont,	/* complain_on_overflow */
	 c33_elf_reloc,		/* special_function */
	 "R_C33_TH",		/* name */
	 false,				/* partial_inplace */
	 0xffff,			/* src_mask */
	 0xffff,			/* dst_mask */
	 false),			/* pcrel_offset */

  /* toff_lo(LABEL(12:0)) */  /* add tazaki 2001.07.24 */
  HOWTO (R_C33_TL,		/* type */
	 0,					/* rightshift */
	 2,					/* size (in bytes) */
	 13,				/* bitsize */
	 false,				/* pc_relative */
	 0,					/* bitpos */
	 complain_overflow_dont,	/* complain_on_overflow */
	 c33_elf_reloc,		/* special_function */
	 "R_C33_TL",		/* name */
	 false,				/* partial_inplace */
	 0xffff,			/* src_mask */
	 0xffff,			/* dst_mask */
	 false),			/* pcrel_offset */

  /* toff_hi(LABEL(25:13)) */  /* add tazaki 2001.11.07 */
  HOWTO (R_C33_ZH,	/* type */
	 0,					/* rightshift */
	 2,					/* size (in bytes) */
	 13,				/* bitsize */
	 false,				/* pc_relative */
	 0,					/* bitpos */
	 complain_overflow_dont,	/* complain_on_overflow */
	 c33_elf_reloc,		/* special_function */
	 "R_C33_ZH",		/* name */
	 false,				/* partial_inplace */
	 0xffff,			/* src_mask */
	 0xffff,			/* dst_mask */
	 false),			/* pcrel_offset */

  /* zoff_lo(LABEL(12:0)) */  /* add tazaki 2001.07.24 */
  HOWTO (R_C33_ZL,		/* type */
	 0,					/* rightshift */
	 2,					/* size (in bytes) */
	 13,				/* bitsize */
	 false,				/* pc_relative */
	 0,					/* bitpos */
	 complain_overflow_dont,	/* complain_on_overflow */
	 c33_elf_reloc,		/* special_function */
	 "R_C33_ZL",		/* name */
	 false,				/* partial_inplace */
	 0xffff,			/* src_mask */
	 0xffff,			/* dst_mask */
	 false),			/* pcrel_offset */

  /* (symbol+imm - dp)@31:19 */
  HOWTO (R_C33_DPH,		/* type */
	 0,					/* rightshift */
	 2,					/* size (in bytes) */
	 13,				/* bitsize */
	 false,				/* pc_relative */
	 0,					/* bitpos */
	 complain_overflow_dont,	/* complain_on_overflow */
	 c33_elf_reloc,		/* special_function */
	 "R_C33_DPH",		/* name */
	 false,				/* partial_inplace */
	 0xffff,			/* src_mask */
	 0xffff,			/* dst_mask */
	 false),			/* pcrel_offset */

  /* (symbol+imm - dp)@18:6 */
  HOWTO (R_C33_DPM,		/* type */
	 0,					/* rightshift */
	 2,					/* size (in bytes) */
	 13,				/* bitsize */
	 false,				/* pc_relative */
	 0,					/* bitpos */
	 complain_overflow_dont,	/* complain_on_overflow */
	 c33_elf_reloc,		/* special_function */
	 "R_C33DP_DPM",		/* name */
	 false,				/* partial_inplace */
	 0xffff,			/* src_mask */
	 0xffff,			/* dst_mask */
	 false),			/* pcrel_offset */

  /* (symbol+imm - dp)@5:0 */
  HOWTO (R_C33_DPL,		/* type */
	 0,					/* rightshift */
	 2,					/* size (in bytes) */
	 6,					/* bitsize */
	 false,				/* pc_relative */
	 0,					/* bitpos */
	 complain_overflow_dont,	/* complain_on_overflow */
	 c33_elf_reloc,		/* special_function */
	 "R_C33_DPL",		/* name */
	 false,				/* partial_inplace */
	 0xffff,			/* src_mask */
	 0xffff,			/* dst_mask */
	 false),			/* pcrel_offset */

  /* loop <LABEL-PC>(4:0) */
  HOWTO (R_C33_LOOP,	/* type */
	 0,					/* rightshift */
	 2,					/* size (in bytes) */
	 4,					/* bitsize */
	 true,				/* pc_relative */
	 0,					/* bitpos */
	 complain_overflow_dont,	/* complain_on_overflow */
	 c33_elf_reloc,		/* special_function */
	 "R_C33_LOOP",		/* name */
	 false,				/* partial_inplace */
	 0xffff,			/* src_mask */
	 0xffff,			/* dst_mask */
	 false),			/* pcrel_offset */

  /* l <LABEL-PC>(8:0) */
  HOWTO (R_C33_JP,	/* type */
	 0,				/* rightshift */
	 2,				/* size (in bytes) */
	 13,				/* bitsize */
	 true,				/* pc_relative */
	 0,				/* bitpos */
	 complain_overflow_dont,	/* complain_on_overflow */
	 c33_elf_reloc,		/* special_function */
	 "R_C33_JP",	/* name */
	 false,				/* partial_inplace */
	 0xffff,			/* src_mask */
	 0xffff,			/* dst_mask */
	 false),			/* pcrel_offset */

/* add T.Tazaki 2002.05.02 sjp,scall,xjp,xcall symbol mask >>> */

  /* @rh <LABEL-PC>(31:22) */
  HOWTO (R_C33_S_RH,	/* type */
	 0,				/* rightshift */
	 2,				/* size (in bytes) */
	 13,				/* bitsize */
	 true,				/* pc_relative */
	 0,				/* bitpos */
	 complain_overflow_dont,	/* complain_on_overflow */
	 c33_elf_reloc,		/* special_function */
	 "R_C33_S_RH",	/* name */
	 false,				/* partial_inplace */
	 0xffff,			/* src_mask */
	 0xffff,			/* dst_mask */
	 false),			/* pcrel_offset */

  /* @rm <LABEL-PC>(21:9) */
  HOWTO (R_C33_S_RM,	/* type */
	 0,				/* rightshift */
	 2,				/* size (in bytes) */
	 13,				/* bitsize */
	 true,				/* pc_relative */
	 0,				/* bitpos */
	 complain_overflow_dont,	/* complain_on_overflow */
	 c33_elf_reloc,		/* special_function */
	 "R_C33_S_RM",	/* name */
	 false,				/* partial_inplace */
	 0xffff,			/* src_mask */
	 0xffff,			/* dst_mask */
	 false),			/* pcrel_offset */

  /* @rl <LABEL-PC>(8:0) */
  HOWTO (R_C33_S_RL,	/* type */
	 0,				/* rightshift */
	 2,				/* size (in bytes) */
	 13,				/* bitsize */
	 true,				/* pc_relative */
	 0,				/* bitpos */
	 complain_overflow_dont,	/* complain_on_overflow */
	 c33_elf_reloc,		/* special_function */
	 "R_C33_S_RL",	/* name */
	 false,				/* partial_inplace */
	 0xffff,			/* src_mask */
	 0xffff,			/* dst_mask */
	 false),			/* pcrel_offset */

/* add T.Tazaki 2002.05.02 sjp,scall,xjp,xcall symbol mask <<< */

  /*  pushn %r0  */  /* add tazaki 2004/08/19 */
  HOWTO (R_C33_PUSHN_R0,	/* type */
	 0,					/* rightshift */
	 2,					/* size (in bytes) */
	 13,				/* bitsize */
	 false,				/* pc_relative */
	 0,					/* bitpos */
	 complain_overflow_dont,	/* complain_on_overflow */
	 c33_elf_reloc,		/* special_function */
	 "R_C33_PUSHN_R0",	/* name */
	 false,				/* partial_inplace */
	 0xffff,			/* src_mask */
	 0xffff,			/* dst_mask */
	 false),			/* pcrel_offset */

  /*  pushn %r1  */  /* add tazaki 2004/08/19 */
  HOWTO (R_C33_PUSHN_R1,	/* type */
	 0,					/* rightshift */
	 2,					/* size (in bytes) */
	 13,				/* bitsize */
	 false,				/* pc_relative */
	 0,					/* bitpos */
	 complain_overflow_dont,	/* complain_on_overflow */
	 c33_elf_reloc,		/* special_function */
	 "R_C33_PUSHN_R1",	/* name */
	 false,				/* partial_inplace */
	 0xffff,			/* src_mask */
	 0xffff,			/* dst_mask */
	 false),			/* pcrel_offset */

  /*  push %r1  */  /* add tazaki 2004/08/19 */
  HOWTO (R_C33_PUSH_R1,	/* type */
	 0,					/* rightshift */
	 2,					/* size (in bytes) */
	 13,				/* bitsize */
	 false,				/* pc_relative */
	 0,					/* bitpos */
	 complain_overflow_dont,	/* complain_on_overflow */
	 c33_elf_reloc,		/* special_function */
	 "R_C33_PUSH_R1",	/* name */
	 false,				/* partial_inplace */
	 0xffff,			/* src_mask */
	 0xffff,			/* dst_mask */
	 false),			/* pcrel_offset */

  /* FDPIC: a word holding the address of SYM's canonical function
     descriptor.  */
  HOWTO (R_C33_FUNCDESC,	/* type */
	 0,				/* rightshift */
	 4,				/* size (in bytes) */
	 32,				/* bitsize */
	 false,				/* pc_relative */
	 0,				/* bitpos */
	 complain_overflow_dont,	/* complain_on_overflow */
	 bfd_elf_generic_reloc,		/* special_function */
	 "R_C33_FUNCDESC",		/* name */
	 false,				/* partial_inplace */
	 0,				/* src_mask */
	 0xffffffff,			/* dst_mask */
	 false),			/* pcrel_offset */

  /* FDPIC: an 8-byte function descriptor for SYM, {entry, %r15}.  Only
     ever a dynamic relocation.  */
  HOWTO (R_C33_FUNCDESC_VALUE,	/* type */
	 0,				/* rightshift */
	 4,				/* size (in bytes) */
	 32,				/* bitsize */
	 false,				/* pc_relative */
	 0,				/* bitpos */
	 complain_overflow_dont,	/* complain_on_overflow */
	 bfd_elf_generic_reloc,		/* special_function */
	 "R_C33_FUNCDESC_VALUE",	/* name */
	 false,				/* partial_inplace */
	 0,				/* src_mask */
	 0xffffffff,			/* dst_mask */
	 false),			/* pcrel_offset */

  /* FDPIC: a word holding the module's own address in the addend,
     relocated through the module's load map.  Only ever a dynamic
     relocation.  */
  HOWTO (R_C33_RELATIVE,	/* type */
	 0,				/* rightshift */
	 4,				/* size (in bytes) */
	 32,				/* bitsize */
	 false,				/* pc_relative */
	 0,				/* bitpos */
	 complain_overflow_dont,	/* complain_on_overflow */
	 bfd_elf_generic_reloc,		/* special_function */
	 "R_C33_RELATIVE",		/* name */
	 false,				/* partial_inplace */
	 0,				/* src_mask */
	 0xffffffff,			/* dst_mask */
	 false),				/* pcrel_offset */

  HOWTO (R_C33_TLS_DTPMOD32, 0, 4, 32, false, 0,
         complain_overflow_dont, bfd_elf_generic_reloc,
         "R_C33_TLS_DTPMOD32", false, 0, 0xffffffff, false),
  HOWTO (R_C33_TLS_DTPREL32, 0, 4, 32, false, 0,
         complain_overflow_dont, bfd_elf_generic_reloc,
         "R_C33_TLS_DTPREL32", false, 0, 0xffffffff, false),
  HOWTO (R_C33_TLS_TPREL32, 0, 4, 32, false, 0,
         complain_overflow_dont, bfd_elf_generic_reloc,
         "R_C33_TLS_TPREL32", false, 0, 0xffffffff, false),
  HOWTO (R_C33_TLS_LE32, 0, 4, 32, false, 0,
         complain_overflow_dont, bfd_elf_generic_reloc,
         "R_C33_TLS_LE32", false, 0, 0xffffffff, false)
};

/* Map BFD reloc types to c33 ELF reloc types.  */

struct c33_elf_reloc_map
{
  /* BFD_RELOC_C33_CALLT_16_16_OFFSET is 258, which will not fix in an
     unsigned char.  */
  bfd_reloc_code_real_type bfd_reloc_val;
  unsigned char elf_reloc_val;
};

static const struct c33_elf_reloc_map c33_elf_reloc_map[] =
{
  { BFD_RELOC_NONE,		R_C33_NONE },
  { BFD_RELOC_32,		R_C33_32 },
  { BFD_RELOC_16,		R_C33_16 },
  { BFD_RELOC_8,		R_C33_8 },
  { BFD_RELOC_C33_AH,	R_C33_AH },
  { BFD_RELOC_C33_AL,	R_C33_AL },
  { BFD_RELOC_C33_RH,	R_C33_RH },
  { BFD_RELOC_C33_RM,	R_C33_RM },
  { BFD_RELOC_C33_RL,	R_C33_RL },
  { BFD_RELOC_C33_S_RH,	R_C33_S_RH },	/* add tazaki 2002.05.02 */
  { BFD_RELOC_C33_S_RM,	R_C33_S_RM },	/* add tazaki 2002.05.02 */
  { BFD_RELOC_C33_S_RL,	R_C33_S_RL },	/* add tazaki 2002.05.02 */
  { BFD_RELOC_C33_JP,	R_C33_JP },	/* add tazaki 2002.05.02 */
  { BFD_RELOC_C33_H,	R_C33_H },
  { BFD_RELOC_C33_M,	R_C33_M },
  { BFD_RELOC_C33_L,	R_C33_L },
  { BFD_RELOC_C33_DH,	R_C33_DH },	/* add tazaki 2002.01.11 */
  { BFD_RELOC_C33_DL,	R_C33_DL }, /* add tazaki 2002.01.11 */
  { BFD_RELOC_C33_GL,	R_C33_GL }, /* add tazaki 2001.07.13 */
  { BFD_RELOC_C33_SH,	R_C33_SH }, /* add tazaki 2001.11.07 */
  { BFD_RELOC_C33_SL,	R_C33_SL }, /* add tazaki 2001.07.13 */
  { BFD_RELOC_C33_TH,	R_C33_TH }, /* add tazaki 2001.11.07 */
  { BFD_RELOC_C33_TL,	R_C33_TL }, /* add tazaki 2001.07.13 */
  { BFD_RELOC_C33_ZH,	R_C33_ZH }, /* add tazaki 2001.11.07 */
  { BFD_RELOC_C33_ZL,	R_C33_ZL }, /* add tazaki 2001.07.13 */
  { BFD_RELOC_C33_DPH,	R_C33_DPH },/* add tazaki 2001.11.08 */
  { BFD_RELOC_C33_DPM,	R_C33_DPM },/* add tazaki 2001.11.08 */
  { BFD_RELOC_C33_DPL,	R_C33_DPL }, /* add tazaki 2001.11.08 */
  { BFD_RELOC_C33_LOOP,	R_C33_LOOP }, /* add tazaki 2002.03.05 */
  { BFD_RELOC_C33_PUSHN_R0,	R_C33_PUSHN_R0 }, /* add tazaki 2004/08/19 */
  { BFD_RELOC_C33_PUSHN_R1,	R_C33_PUSHN_R1 }, /* add tazaki 2004/08/19 */
  { BFD_RELOC_C33_PUSH_R1,	R_C33_PUSH_R1 },  /* add tazaki 2004/08/19 */
  { BFD_RELOC_C33_FUNCDESC,	R_C33_FUNCDESC },
  { BFD_RELOC_C33_FUNCDESC_VALUE, R_C33_FUNCDESC_VALUE },
  { BFD_RELOC_C33_TLS_DTPMOD32, R_C33_TLS_DTPMOD32 },
  { BFD_RELOC_C33_TLS_DTPREL32, R_C33_TLS_DTPREL32 },
  { BFD_RELOC_C33_TLS_TPREL32, R_C33_TLS_TPREL32 },
  { BFD_RELOC_C33_TLS_LE32, R_C33_TLS_LE32 }
};


/* Map a bfd relocation into the appropriate howto structure */
static reloc_howto_type *
c33_elf_reloc_type_lookup (bfd * abfd ATTRIBUTE_UNUSED,
                           bfd_reloc_code_real_type code)
{
  unsigned int i;

  for (i = 0;
       i < sizeof (c33_elf_reloc_map) / sizeof (struct c33_elf_reloc_map);
       i++)
    {
      if (c33_elf_reloc_map[i].bfd_reloc_val == code)
	{
	  BFD_ASSERT (c33_elf_howto_table[c33_elf_reloc_map[i].elf_reloc_val].type == c33_elf_reloc_map[i].elf_reloc_val);
	  
	  return & c33_elf_howto_table[c33_elf_reloc_map[i].elf_reloc_val];
	}
    }

  return NULL;
}


/* Set the howto pointer for an c33 ELF reloc.  */
static bool
c33_elf_info_to_howto_rel (bfd * abfd,
                           arelent * cache_ptr,
                           Elf_Internal_Rela * dst)
{
  unsigned int r_type = ELF32_R_TYPE (dst->r_info);

  if (r_type >= (unsigned int) R_C33_max)
    {
      /* xgettext:c-format */
      _bfd_error_handler (_("%pB: unsupported relocation type %#x"),
			  abfd, r_type);
      bfd_set_error (bfd_error_bad_value);
      return false;
    }

  cache_ptr->howto = &c33_elf_howto_table[r_type];
  return true;
}

/* Set the howto pointer for a C33 ELF reloc (type RELA). */
static bool
c33_elf_info_to_howto_rela (bfd * abfd,
                            arelent * cache_ptr,
                            Elf_Internal_Rela *dst)
{
  unsigned int r_type = ELF32_R_TYPE (dst->r_info);

  if (r_type >= (unsigned int) R_C33_max)
    {
      /* xgettext:c-format */
      _bfd_error_handler (_("%pB: unsupported relocation type %#x"),
			  abfd, r_type);
      bfd_set_error (bfd_error_bad_value);
      return false;
    }

  cache_ptr->howto = &c33_elf_howto_table[r_type];
  return true;
}


/* Look through the relocs for a section during the first phase, and
   allocate space in the global offset table or procedure linkage
   table.  */

static bool
c33_elf_check_relocs (bfd * abfd,
                      struct bfd_link_info * info,
                      asection * sec,
                      const Elf_Internal_Rela * relocs)
{
  bool ret = true;
  bfd *dynobj;
  Elf_Internal_Shdr *symtab_hdr;
  struct elf_link_hash_entry **sym_hashes;
  const Elf_Internal_Rela *rel;
  const Elf_Internal_Rela *rel_end;
  asection *sreloc;
  enum c33_reloc_type r_type;



  if (bfd_link_relocatable (info))
    return true;

#ifdef DEBUG
  fprintf (stderr, "c33_elf_check_relocs called for section %s in %s\n",
	   bfd_section_name (sec),
	   bfd_get_filename (abfd));
#endif

  dynobj = elf_hash_table (info)->dynobj;
  symtab_hdr = &elf_tdata (abfd)->symtab_hdr;
  sym_hashes = elf_sym_hashes (abfd);
  sreloc = NULL;

  rel_end = relocs + sec->reloc_count;
  for (rel = relocs; rel < rel_end; rel++)
    {
      unsigned long r_symndx;
      struct elf_link_hash_entry *h;

      r_symndx = ELF32_R_SYM (rel->r_info);
      if (r_symndx < symtab_hdr->sh_info)
			h = NULL;
      else
			h = sym_hashes[r_symndx - symtab_hdr->sh_info];

      r_type = (enum c33_reloc_type) ELF32_R_TYPE (rel->r_info);
      switch (r_type)
		{
			default:
			case R_C33_NONE:
			case R_C33_32:
			case R_C33_16:
			case R_C33_8:
			case R_C33_AH:
			case R_C33_AL:
			case R_C33_RH:
			case R_C33_RM:
			case R_C33_RL:
			case R_C33_S_RH:	/* add tazaki 2002.05.02 */
			case R_C33_S_RM:	/* add tazaki 2002.05.02 */
			case R_C33_S_RL:	/* add tazaki 2002.05.02 */
			case R_C33_JP:	/* add tazaki 2002.04.22 */
			case R_C33_H:
			case R_C33_M:
			case R_C33_L:
			/* >>>>> add tazaki 2002.03.05 */
			case R_C33_DH:
			case R_C33_DL:
			case R_C33_GL:
			case R_C33_SH:
			case R_C33_SL:
			case R_C33_TH:
			case R_C33_TL:
			case R_C33_ZH:
			case R_C33_ZL:
			case R_C33_DPH:
			case R_C33_DPM:
			case R_C33_DPL:
			case R_C33_LOOP:
			/* <<<<< add tazaki 2002.03.05 */
			case R_C33_PUSHN_R0:	/* add T.Tazaki 2004/08/19 */
			case R_C33_PUSHN_R1:	/* add T.Tazaki 2004/08/19 */
			case R_C33_PUSH_R1:		/* add T.Tazaki 2004/08/19 */
			  break;

		}
    }

  return ret;
}

/*
 * In the old version, when an entry was checked out from the table,
 * it was deleted.  This produced an error if the entry was needed
 * more than once, as the second attempted retry failed.
 *
 * In the current version, the entry is not deleted, instead we set
 * the field 'found' to true.  If a second lookup matches the same
 * entry, then we know that the ah reloc has already been updated
 * and does not need to be updated a second time.
 *
 * TODO - TOFIX: If it is possible that we need to restore 2 different
 * addresses from the same table entry, where the first generates an
 * overflow, whilst the second do not, then this code will fail.
 */

typedef struct ah_location
{
  bfd_vma       addend;
  bfd_byte *    address;
  unsigned long counter;
  bool       found;
  struct ah_location * next;
}
ah_location;

static ah_location *  previous_ah;
static ah_location *  free_ah;
static unsigned long     ah_counter;

#if 0
static void
remember_ah_reloc (bfd * abfd, bfd_vma addend, bfd_byte * address)
{
  ah_location * entry = NULL;
  
  /* Find a free structure.  */
  if (free_ah == NULL)
    free_ah = (ah_location *) bfd_zalloc (abfd, sizeof (* free_ah));

  entry      = free_ah;
  free_ah = free_ah->next;
  
  entry->addend  = addend;
  entry->address = address;
  entry->counter = ah_counter ++;
  entry->found   = false;
  entry->next    = previous_ah;
  previous_ah = entry;
  
  /* Cope with wrap around of our counter.  */
  if (ah_counter == 0)
    {
      /* XXX - Assume that all counter entries differ only in their low 16 bits.  */
      for (entry = previous_ah; entry != NULL; entry = entry->next)
	entry->counter &= 0xffff;

      ah_counter = 0x10000;
    }
  
  return;
}

static bfd_byte *
find_remembered_ah_reloc (bfd_vma addend, bfd_byte * address)
{
  ah_location * entry;
  
  if( previous_ah == NULL ){	/* add tazaki 2001.11.01 */
      return NULL;
  }

  /* Search the table.  Record the most recent entry that matches.  */
	entry = previous_ah;
	
	/* ���O��ext @ah�����݂��邩 */
	if ((entry->addend == addend)  && (entry->address == address-2))
	{
		return entry->address;
	}
	else {
		return NULL;
	}
}

#endif

/* >>>>>>>>>>  add tazaki 2002.03.04              */
bfd_byte * g_doff_hi_address = NULL;
bfd_byte * g_soff_hi_address = NULL;
bfd_byte * g_toff_hi_address = NULL;
bfd_byte * g_zoff_hi_address = NULL;
bfd_byte * g_symbol_mask_ah_address = NULL;
bfd_byte * g_symbol_mask_rh_address = NULL;
bfd_byte * g_symbol_mask_rm_address = NULL;
bfd_byte * g_dpoff_h_address = NULL;
bfd_byte * g_dpoff_m_address = NULL;
/* <<<<<<<<<<  add tazaki 2002.03.04              */


/* �������̃V���{�����m�肷�� */
/* FIXME:  The code here probably ought to be removed and the code in reloc.c
   allowed to do its  stuff instead.  At least for most of the relocs, anwyay.  */
static bfd_reloc_status_type
c33_elf_perform_relocation (abfd, r_type, addend, address,gp)
     bfd *      abfd;
     int        r_type;
     bfd_vma    addend;
     bfd_byte * address;
	unsigned long	gp;		/* DA,SDA,TDA,ZDA */

{
	unsigned long insn;
	bfd_signed_vma saddend = (bfd_signed_vma) addend;
  
	switch (r_type)
    {
		case R_C33_32:
      		bfd_put_32 (abfd, addend, address);
      		return bfd_reloc_ok;

		case R_C33_16:
			addend += bfd_get_16 (abfd, address);
			saddend = (bfd_signed_vma) addend;
      		if (saddend > 0x7fff || saddend < -0x8000)
				return bfd_reloc_overflow;
      		insn = addend;
      		break;

		case R_C33_8:
      		addend += (char) bfd_get_8 (abfd, address);

      		saddend = (bfd_signed_vma) addend;
      		if (saddend > 0x7f || saddend < -0x80)
				return bfd_reloc_overflow;

      		bfd_put_8 (abfd, addend, address);
      		return bfd_reloc_ok;

    	case R_C33_AH:	/* @ah (25:13) */
			/* Remember where this relocation took place.  */
		    g_symbol_mask_ah_address = address;

			/* Get Instruction code */
			insn = bfd_get_16(abfd, address);
			/* over 26bit ? */
			if ( addend > 0x3ffffff )
				return bfd_reloc_outofrange;

			/* add symbol25:13 to insn */
			insn += ((addend >> 13) & 0x1fff);
			break;

    	case R_C33_AL:	/* @al (12:0) */
			insn = bfd_get_16(abfd, address);

			if (g_symbol_mask_ah_address != (address - 2)){
				/* over 13bit ? */
	      		if (addend > 0x1fff)
						return bfd_reloc_outofrange;
			}

			insn += (addend & 0x1fff);
			break;

		case R_C33_RH:	 /* LABEL-PC(31:22) */
		case R_C33_S_RH: /* LABEL-PC(31:22) */ /* add T.Tazaki 2002.05.02 */
			/* Remember where this relocation took place.  */
		    g_symbol_mask_rh_address = address;

			insn = bfd_get_16(abfd, address);
			insn += (((addend - 4) >> 19) & 0x1ff8);
			break;
		
		case R_C33_RM:	/* LABEL-PC(21:9)  */
		case R_C33_S_RM: /* LABEL-PC(31:22) */ /* add T.Tazaki 2002.05.02 */
			/* Remember where this relocation took place.  */
		    g_symbol_mask_rm_address = address;

			insn = bfd_get_16(abfd, address);

			/* if exist @rh before ext ? */
			if (g_symbol_mask_rh_address != (address - 2)){
				saddend = (bfd_signed_vma) addend;
				/* over signed 22bit ? */
		      	if ((saddend - 2) > 0x1ffffe || (saddend - 2 ) < -0x200000 ) /* tazaki 2002.04.24 */
						return bfd_reloc_outofrange;
			}
			insn += (((addend - 2) >> 9) & 0x1fff);
			break;

		case R_C33_JP:	/* LABEL-PC(8:0)   */
		case R_C33_RL:	/* LABEL-PC(8:0)   */
		case R_C33_S_RL: /* LABEL-PC(31:22) */ /* add T.Tazaki 2002.05.02 */
						/* sign8=sign32(8:1) sign32(0)=0 */
			insn = bfd_get_16(abfd, address);
			if ((addend & 1) != 0)
				return bfd_reloc_outofrange;

			/* if exist @rm before ext ? */
			if (g_symbol_mask_rm_address != (address - 2)){
				
				saddend = (bfd_signed_vma) addend;
				/* over signed 8bit ? */
	      		if (saddend > 254 || saddend < -256)
						return bfd_reloc_outofrange;
			}

			insn += ((addend >> 1) & 0xff);
			break;

		case R_C33_H:	/* LABEL(31:19) */
			insn = bfd_get_16(abfd, address);
			insn += ((addend >> 19) & 0x1fff);
			break;

		case R_C33_M:	/* LABEL(18:6)  */
			insn = bfd_get_16(abfd, address);
			insn += ((addend >> 6) & 0x1fff);
			break;

		case R_C33_L:	/* LABEL(5:0)   */
						/* ld.w %rd,LABEL@l */
			insn = bfd_get_16(abfd, address);
			insn += (addend & 0x3f) << 4;
			break;

	    case R_C33_DH:	/* doff_hi(LABEL) (25:13) */
			/* default=0x0 & Warning display */

			/* Remember where this relocation took place.  */
		    g_doff_hi_address = address;

			/* Get Instruction code */
			insn = bfd_get_16(abfd, address);

			if (addend >= gp)
				addend -= gp;
			else
				return bfd_reloc_over_doff_globalpointer;

			/* over 26bit ? */
			if (addend > 0x3ffffff)
				return bfd_reloc_doff_over_64mb;

			/* add symbol25:13 to insn */
			insn += ((addend >> 13) & 0x1fff);
			break;
			
	    case R_C33_DL:	/* doff_lo(LABEL) (12:0) */
			insn = bfd_get_16(abfd, address);

			if (addend >= gp)
				addend -= gp;
			else{
				return bfd_reloc_over_doff_globalpointer;
			}

			/* over 13bit ? */
			if (addend > 0x1fff){
				/* if exist doff_hi() before ext ? */
				if (g_doff_hi_address != (address - 2)){
					return bfd_reloc_doff_over_8kb;
				}
			}
			
			insn += (addend & 0x1fff);
			break;

			
	    case R_C33_GL:	/* goff_lo(LABEL) (12:0) */
			insn = bfd_get_16(abfd, address);

			if (addend >= gp)
				addend -= gp;
			else{
				return bfd_reloc_over_goff_globalpointer;
			}

			/* over 13bit ? */
			if (addend > 0x1fff){
				return bfd_reloc_goff_over_8kb;
			}
			
			insn += (addend & 0x1fff);
			break;

	    case R_C33_SH:	/* soff_hi(LABEL) (25:13) */

			/* Remember where this relocation took place.  */
		    g_soff_hi_address = address;

			/* Get Instruction code */
			insn = bfd_get_16(abfd, address);

			if (addend >= gp)
				addend -= gp;
			else
				return bfd_reloc_over_soff_globalpointer;

			/* over 26bit ? */
			if (addend > 0x3ffffff)
				return bfd_reloc_soff_over_64mb;

			/* add symbol25:13 to insn */
			insn += ((addend >> 13) & 0x1fff);
			break;
			
	    case R_C33_SL:	/* soff_lo(LABEL) (12:0) */

			insn = bfd_get_16(abfd, address);

			if (addend >= gp)
				addend -= gp;
			else
				return bfd_reloc_over_soff_globalpointer;

			if (addend > 0x1fff){
				/* if exist soff_hi() before ext ? */
				if (g_soff_hi_address != (address - 2)){
					return bfd_reloc_soff_over_8kb;
				}
			}

			insn += (addend & 0x1fff);
			break;

	    case R_C33_TH:	/* toff_hi(LABEL) (25:13) */

			/* Remember where this relocation took place.  */
		    g_toff_hi_address = address;

			/* Get Instruction code */
			insn = bfd_get_16(abfd, address);

			if (addend >= gp)
				addend -= gp;
			else
				return bfd_reloc_over_toff_globalpointer;

			/* over 26bit ? */
			if (addend > 0x3ffffff)
				return bfd_reloc_toff_over_64mb;

			/* add symbol25:13 to insn */
			insn += ((addend >> 13) & 0x1fff);
			break;
			
	    case R_C33_TL:	/* toff_lo(LABEL) (12:0) */

			/* Get Instruction code */
			insn = bfd_get_16(abfd, address);

			if (addend >= gp)
				addend -= gp;
			else
				return bfd_reloc_over_toff_globalpointer;

			/* over 13bit ? */
			if (addend > 0x1fff){
				/* if exist toff_hi() before ext ? */
				if (g_toff_hi_address != (address - 2)){
					return bfd_reloc_toff_over_8kb;
				}
			}

			insn += (addend & 0x1fff);
			break;

	    case R_C33_ZH:	/* zoff_hi(LABEL) (25:13) */

			/* Remember where this relocation took place.  */
		    g_zoff_hi_address = address;

			/* Get Instruction code */
			insn = bfd_get_16(abfd, address);

			if (addend >= gp)
				addend -= gp;
			else
				return bfd_reloc_over_zoff_globalpointer;

			/* over 26bit ? */
			if (addend > 0x3ffffff)
				return bfd_reloc_zoff_over_64mb;

			/* add symbol25:13 to insn */
			insn += ((addend >> 13) & 0x1fff);
			break;
			
	    case R_C33_ZL:	/* zoff_lo(LABEL) (12:0) */

			insn = bfd_get_16(abfd, address);

			if (addend >= gp)
				addend -= gp;
			else
				return bfd_reloc_over_zoff_globalpointer;

			/* over 13bit ? */
			if (addend > 0x1fff){
				/* if exist zoff_hi() before ext ? */
				if (g_zoff_hi_address != (address - 2)){
					return bfd_reloc_zoff_over_8kb;
				}
			}

			insn += (addend & 0x1fff);
			break;

		case R_C33_DPH:	/* (symbol+imm - %dp)@31:19 */

			/* Remember where this relocation took place.  */
		    g_dpoff_h_address = address;

			if (addend >= gp)
				addend -= gp;
			else
				return bfd_reloc_over_doff_globalpointer;

			/* Get Instruction code */
			insn = bfd_get_16(abfd, address);
			insn += ((addend >> 19) & 0x1fff);
			break;

		case R_C33_DPM:	/* (symbol+imm - %dp)@18:6 */
			/* Remember where this relocation took place.  */
		    g_dpoff_m_address = address;

			if (addend >= gp)
				addend -= gp;
			else
				return bfd_reloc_over_doff_globalpointer;

			/* over 19bit ? */
			if (addend > 0x7ffff){
				/* if exist dpoff_h() before ext ? */
				if (g_dpoff_h_address != (address - 2)){
					return bfd_reloc_dpoff_over_512kb;
				}
			}

			insn = bfd_get_16(abfd, address);
			insn += ((addend >> 6) & 0x1fff);
			break;

		case R_C33_DPL:	/* (symbol+imm - %dp)@5:0 */
			if (addend >= gp)
				addend -= gp;
			else
				return bfd_reloc_over_doff_globalpointer;

			/* over 6bit ? */
			if (addend > 0x3f){
				/* if exist dpoff_m() before ext ? */
				if (g_dpoff_m_address != (address - 2)){
					return bfd_reloc_dpoff_over_64b;
				}
			}

			insn = bfd_get_16(abfd, address);
			insn += (addend & 0x3f) << 4;
			break;

		case R_C33_LOOP:	/* LABEL-PC(4:0)   */
			/* imm5=imm4(4:1) */
			insn = bfd_get_16(abfd, address);

			saddend = (bfd_signed_vma)(addend - *address);
			/* over imm4bit ? */
      		if (saddend > 30 || saddend < 0)
				return bfd_reloc_outofrange;

			/* "   loop %rc,Label-2 " �\���͎g�p���h���̂ŁA "loop %rc,Label" ���\�Ƃ��邽��-�P����B add T.Tazaki 2004/09/22 >>> */
			/* "   nop              " */
			/* "   nop              " */
			/* "Label:              " */
			
//			insn += (((addend - *address) & 0x1e) << 3);
			insn += ((((addend - *address)-1) & 0x1e) << 3);

			/* add T.Tazaki 2004/09/22 <<< */
			break;

		/* add T.Tazaki 2004/08/19 >>> */
	    case R_C33_PUSHN_R0:	/* xld,xbtst �W�J��̍ŏ��́@"pushn %r0 */
			insn = bfd_get_16(abfd, address);

			insn = 0x0200;	/* pushn %r0 */
			break;
	    case R_C33_PUSHN_R1:	/* xld,xbtst �W�J��̍ŏ��́@"pushn %r1 */
			insn = bfd_get_16(abfd, address);

			insn = 0x0201;	/* pushn %r1 */
			break;
	    case R_C33_PUSH_R1:		/* ADV or PE  xld,xbtst �W�J��̍ŏ��́@"push %r1 */
			insn = bfd_get_16(abfd, address);

			insn = 0x0011;	/* push %r1 */
			break;
		/* add T.Tazaki 2004/08/19 <<< */


    	default:
      		/* fprintf (stderr, "reloc type %d not SUPPORTED\n", r_type ); */
      		return bfd_reloc_notsupported;

	}
  	
  	bfd_put_16 (abfd, insn, address);
  	return bfd_reloc_ok;
}



/* Insert the addend into the instruction.  */
static bfd_reloc_status_type
c33_elf_reloc (bfd * abfd ATTRIBUTE_UNUSED,
               arelent * reloc,
               asymbol * symbol,
               void * data ATTRIBUTE_UNUSED,
               asection * isection,
               bfd * obfd,
               char ** err ATTRIBUTE_UNUSED)
{
  long relocation;
  
  /* If there is an output BFD we are being called from
     bfd_install_relocation while the assembler writes the object, not
     from the final link: the reloc is being handed on, so leave the
     addend alone and only fix up the address.

     Every howto in this file is partial_inplace = false, which means the
     whole value lives in the addend and there is nothing to deposit in
     the section contents here.

     This used to also require the symbol not to be a section symbol,
     copied from bfd_elf_generic_reloc -- but that function returns
     bfd_reloc_continue for section symbols and lets
     bfd_install_relocation finish the job, and bfd_install_relocation
     only subtracts the reloc's own address when partial_inplace is set.
     Falling through to the final-link code below instead left the addend
     holding a complete "symbol - PC", which ld then relocated a second
     time.  gas reduces any *local* symbol to a section symbol plus
     addend, so the visible symptom was that an xcall or xjp to a static
     function in another section -- a static function called from
     .text.startup, say -- branched to a wild address, while the same
     call to a global function was fine.  */
  if (obfd != (bfd *) NULL
      && (! reloc->howto->partial_inplace
	  || ((symbol->flags & BSF_SECTION_SYM) == 0
	      && reloc->addend == 0)))
    {
      reloc->address += isection->output_offset;
      return bfd_reloc_ok;
    }
#if 0  
  else if (obfd != NULL)
    {
      return bfd_reloc_continue;
    }
#endif
  
  /* Catch relocs involving undefined symbols.  */
  if (bfd_is_und_section (symbol->section)
      && (symbol->flags & BSF_WEAK) == 0
      && obfd == NULL)
    return bfd_reloc_undefined;

  /* We handle final linking of some relocs ourselves.  */

  /* Is the address of the relocation really within the section?  */
  if (reloc->address > isection->size)
    return bfd_reloc_outofrange;
  
  /* Work out which section the relocation is targetted at and the
     initial relocation command value.  */
  
  /* Get symbol value.  (Common symbols are special.)  */
  if (bfd_is_com_section (symbol->section))
    relocation = 0;
  else
    relocation = symbol->value;
  
  /* Convert input-section-relative symbol value to absolute + addend.  */
  relocation += symbol->section->output_section->vma;
  relocation += symbol->section->output_offset;
  relocation += reloc->addend;
  
  if (reloc->howto->pc_relative == true)
    {
      /* Here the variable relocation holds the final address of the
	 symbol we are relocating against, plus any addend.  */
      relocation -= isection->output_section->vma + isection->output_offset;
      
      /* Deal with pcrel_offset */
      relocation -= reloc->address;
    }

  reloc->addend = relocation;	
  return bfd_reloc_ok;
}


/* Check local label name */
/*ARGSUSED*/
static bool
c33_elf_is_local_label_name (bfd * abfd ATTRIBUTE_UNUSED, const char * name)
{
  /* Keep the legacy compiler's __L convention, and accept the standard ELF
     .L spelling used by modern GCC and GAS.  The latter is important for
     section-symbol relocation policy and for discarding temporary symbols. */
  return ((name[0] == '_' && name[1] == '_' && name[2] == 'L')
          || _bfd_elf_is_local_label_name (abfd, name));
}


/* The symbol changed by relocation at the time of a link is solved. */

int	i_dp_warn_flag = 0;		/* add tazaki 2002.01.11 */
int	i_gdp_warn_flag = 0;	/* add tazaki 2002.01.11 */
int	i_sdp_warn_flag = 0;	/* add tazaki 2002.01.11 */
int	i_tdp_warn_flag = 0;	/* add tazaki 2002.01.11 */
int	i_zdp_warn_flag = 0;	/* add tazaki 2002.01.11 */

/* Perform a relocation as part of a final link.  */
static bfd_reloc_status_type
c33_elf_final_link_relocate (howto, input_bfd, output_bfd,
				    input_section, contents, offset, value,
				    addend, info, sym_sec, is_local)
     reloc_howto_type *      howto;
     bfd *                   input_bfd;
     bfd *                   output_bfd ATTRIBUTE_UNUSED;
     asection *              input_section;
     bfd_byte *              contents;
     bfd_vma                 offset;
     bfd_vma                 value;
     bfd_vma                 addend;
     struct bfd_link_info *  info;
     asection *              sym_sec;
     int                     is_local ATTRIBUTE_UNUSED;
{
  unsigned long  r_type   = howto->type;
  bfd_byte *     hit_data = contents + offset;
		unsigned long                gp = 0;
		struct bfd_link_hash_entry * h;

  /* Adjust the value according to the relocation.  */
  switch (r_type)
    {
    case R_C33_16:
    case R_C33_32:
    case R_C33_8:
	case R_C33_H:
	case R_C33_M:
	case R_C33_L:
    case R_C33_AH:
    case R_C33_AL:
		break;

   	case R_C33_RH:
	case R_C33_RM:
	case R_C33_RL:
   	case R_C33_S_RH:
	case R_C33_S_RM:
	case R_C33_S_RL:
	case R_C33_JP:
	case R_C33_LOOP:
		/* set PC relative value */
		value -= (input_section->output_section->vma
		+ input_section->output_offset);
		value -= offset;
		break;

	case R_C33_PUSHN_R0:		/* add T.Tazaki 2004/08/19 */
	case R_C33_PUSHN_R1:		/* add T.Tazaki 2004/08/19 */
	case R_C33_PUSH_R1:			/* add T.Tazaki 2004/08/19 */
    case R_C33_NONE:
      return bfd_reloc_ok;

	/* add tazaki 2001.08.02 >>>>> */

	case R_C33_DH:
	case R_C33_DL:

		/* Get the value of __dp.  */
		h = bfd_link_hash_lookup (info->hash, "__dp", false, false, true);
		if (h == (struct bfd_link_hash_entry *) NULL
		    || h->type != bfd_link_hash_defined)
		{
			gp = 0;
			if( i_dp_warn_flag == 0 ){
				fprintf( stderr,"Warning: __dp symbol cannot be refered to.\n" );
				i_dp_warn_flag = 1;
			}
		}
		else {
			gp = (h->u.def.value
		      + h->u.def.section->output_section->vma
		      + h->u.def.section->output_offset);
		}

      	break;

	case R_C33_GL:

		/* Get the value of __gdp.  */
		h = bfd_link_hash_lookup (info->hash, "__gdp", false, false, true);
		if (h == (struct bfd_link_hash_entry *) NULL
		    || h->type != bfd_link_hash_defined)
		{
			gp = 0;
			if( i_gdp_warn_flag == 0 ){
				fprintf( stderr,"Warning: __gdp symbol cannot be refered to.\n" );
				i_gdp_warn_flag = 1;
			}
		}
		else {
			gp = (h->u.def.value
		      + h->u.def.section->output_section->vma
		      + h->u.def.section->output_offset);
		}

      	break;

	case R_C33_SH:
	case R_C33_SL:
		/* Get the value of __sdp.  */
		h = bfd_link_hash_lookup (info->hash, "__sdp", false, false, true);
		if (h == (struct bfd_link_hash_entry *) NULL
		    || h->type != bfd_link_hash_defined)
		{
			gp = 0;
			if( i_sdp_warn_flag == 0 ){
				fprintf( stderr,"Warning: __sdp symbol cannot be refered to.\n" );
				i_sdp_warn_flag = 1;
			}
		}else{
			gp = (h->u.def.value
			      + h->u.def.section->output_section->vma
			      + h->u.def.section->output_offset);
		}
		break;

	case R_C33_TH:
	case R_C33_TL:
		/* Get the value of __tdp.  */
		h = bfd_link_hash_lookup (info->hash, "__tdp", false, false, true);
		if (h == (struct bfd_link_hash_entry *) NULL
		    || h->type != bfd_link_hash_defined)
		{
			gp = 0;
			if( i_tdp_warn_flag == 0 ){
				fprintf( stderr,"Warning: __tdp symbol cannot be refered to.\n" );
				i_tdp_warn_flag = 1;
			}
		}else{
			gp = (h->u.def.value
			      + h->u.def.section->output_section->vma
			      + h->u.def.section->output_offset);
		}
		break;

	case R_C33_ZH:
	case R_C33_ZL:
		/* Get the value of __zdp.  */
		h = bfd_link_hash_lookup (info->hash, "__zdp", false, false, true);
		if (h == (struct bfd_link_hash_entry *) NULL
		    || h->type != bfd_link_hash_defined)
		{
			gp = 0;
			if( i_zdp_warn_flag == 0 ){
				fprintf( stderr,"Warning: __zdp symbol cannot be refered to.\n" );
				i_zdp_warn_flag = 1;
			}
		}else{
			gp = (h->u.def.value
			      + h->u.def.section->output_section->vma
			      + h->u.def.section->output_offset);
		}
		break;

	case R_C33_DPH:
	case R_C33_DPM:
	case R_C33_DPL:

		/* Get the value of __dp.  */
		h = bfd_link_hash_lookup (info->hash, "__dp", false, false, true);
		if (h == (struct bfd_link_hash_entry *) NULL
		    || h->type != bfd_link_hash_defined)
		{
			gp = 0;
			if( i_dp_warn_flag == 0 ){
				fprintf( stderr,"Warning: __dp symbol cannot be refered to.\n" );
				i_dp_warn_flag = 1;
			}
		}
		else {
			gp = (h->u.def.value
		      + h->u.def.section->output_section->vma
		      + h->u.def.section->output_offset);
		}

      	break;

	default:
	    	return bfd_reloc_notsupported;
	}


	/* add tazaki 2001.08.02 <<<<< */

  /* Perform the relocation.  */
  return c33_elf_perform_relocation (input_bfd, r_type, value + addend, hit_data,gp); 
}


static reloc_howto_type *
c33_elf_reloc_name_lookup (bfd * abfd ATTRIBUTE_UNUSED,
			   const char * r_name)
{
  unsigned int i;

  for (i = 0; i < ARRAY_SIZE (c33_elf_howto_table); i++)
    if (c33_elf_howto_table[i].name != NULL
	&& strcasecmp (c33_elf_howto_table[i].name, r_name) == 0)
      return &c33_elf_howto_table[i];

  return NULL;
}

/* ------------------------------------------------------------------------
   FDPIC: no-MMU Linux executables and shared libraries whose text and data
   segments the kernel and the dynamic linker place independently.

   -mfdpic code is -msep-data code: its text holds no address, %r15 points
   at its module's data segment (__dp, where .got starts), and every address
   it needs is a word in that segment.  What FDPIC adds:

   - A call to a function another module may define goes through a .plt
     entry, which loads the callee's descriptor from .got, {entry, the
     callee module's %r15}, sets %r15 and jumps.  The caller restores its
     own %r15 after the call.  The ABI notes are in gcc/ABI.md.

   - A function pointer is the address of a canonical descriptor.  An
     R_C33_FUNCDESC word asks for one: a descriptor in this module's .got
     when the function is its own and not exported, otherwise a dynamic
     relocation, and ld.so makes one.

   - Every word holding an address of this module is corrected at load
     time by its segment's displacement.  An executable lists them in
     .rofixup and relocates itself at startup (crt1's __self_reloc); a
     shared library gets an R_C33_32 dynamic relocation without a symbol,
     whose addend is the link-time address.  Either way the last .rofixup
     entry is the link-time address of __dp, which __self_reloc returns
     relocated: that is how the startup code finds its %r15.

   Words naming symbols of other modules are dynamic relocations: R_C33_32
   against the symbol, R_C33_FUNCDESC for a function pointer and
   R_C33_FUNCDESC_VALUE for a .plt entry's descriptor.  Lazy binding needs
   the two words of a descriptor read atomically, which nothing guarantees
   here, so ld.so resolves every descriptor at load.  */

extern const bfd_target c33_elf32_fdpic_vec;
#define IS_FDPIC(bfd) ((bfd)->xvec == &c33_elf32_fdpic_vec)

#define ELF_DYNAMIC_INTERPRETER	"/lib/ld-uClibc.so.0"
/* A dynamic module's .got starts with words for ld.so: the third holds
   the module's struct elf_resolve, which it finds from a descriptor's %r15.
   Sixteen bytes keep the descriptors after it 8-byte aligned.  */
#define C33FDPIC_GOT_HEADER_SIZE 16
/* .plt starts with a trampoline to ld.so's lazy resolver, and each entry is
   the 16-byte call through a descriptor followed by the 12-byte stub its
   descriptor points at until the resolver has filled it in.  */
#define C33FDPIC_PLT_HEADER_SIZE 8
#define C33FDPIC_PLT_ENTRY_SIZE	28
#define C33FDPIC_PLT_LAZY_OFFSET 16
#define C33FDPIC_FD_SIZE	8
#define C33FDPIC_NONE		((bfd_vma) -1)
#define C33FDPIC_WANTED		((bfd_vma) -2)

struct c33fdpic_link_hash_entry
{
  struct elf_link_hash_entry elf;
  /* A PC-relative call or jump names the symbol.  */
  unsigned int call : 1;
  /* An R_C33_FUNCDESC names the symbol.  */
  unsigned int fd : 1;
  /* Its .plt entry and the descriptor in .got the entry loads.  */
  bfd_vma plt_offset;
  bfd_vma plt_got;
  /* Its canonical descriptor in .got, when this module owns it.  */
  bfd_vma fd_got;
};

#define c33fdpic_entry(h) ((struct c33fdpic_link_hash_entry *) (h))

struct c33fdpic_link_hash_table
{
  struct elf_link_hash_table elf;
  asection *srofixup;
  /* Entries written so far; they must fill the sections exactly.  */
  bfd_vma rofixup_count;
  bfd_vma rela_count;
  bfd_vma relplt_count;
  /* The fixups were counted again after .eh_frame was edited.  */
  bool recounted;
};

#define c33fdpic_hash_table(info) \
  ((struct c33fdpic_link_hash_table *) ((info)->hash))

enum c33fdpic_word
{
  C33FDPIC_WORD_PLAIN,		/* Nothing to do at load time.  */
  C33FDPIC_WORD_ROFIXUP,	/* An executable's own address.  */
  C33FDPIC_WORD_RELATIVE,	/* A shared library's own address.  */
  C33FDPIC_WORD_SYMBOLIC	/* Resolved by ld.so against a symbol.  */
};

static struct bfd_hash_entry *
c33fdpic_link_hash_newfunc (struct bfd_hash_entry *entry,
			    struct bfd_hash_table *table, const char *string)
{
  if (entry == NULL)
    {
      entry = bfd_hash_allocate (table,
				 sizeof (struct c33fdpic_link_hash_entry));
      if (entry == NULL)
	return NULL;
    }
  entry = _bfd_elf_link_hash_newfunc (entry, table, string);
  if (entry != NULL)
    {
      struct c33fdpic_link_hash_entry *e
	= (struct c33fdpic_link_hash_entry *) entry;

      e->call = 0;
      e->fd = 0;
      e->plt_offset = C33FDPIC_NONE;
      e->plt_got = C33FDPIC_NONE;
      e->fd_got = C33FDPIC_NONE;
    }
  return entry;
}

static struct bfd_link_hash_table *
c33fdpic_link_hash_table_create (bfd *abfd)
{
  struct c33fdpic_link_hash_table *ret;

  ret = bfd_zmalloc (sizeof (*ret));
  if (ret == NULL)
    return NULL;
  if (!_bfd_elf_link_hash_table_init (&ret->elf, abfd,
				      c33fdpic_link_hash_newfunc,
				      sizeof (struct c33fdpic_link_hash_entry)))
    {
      free (ret);
      return NULL;
    }
  return &ret->elf.root;
}

static void
c33fdpic_copy_indirect_symbol (struct bfd_link_info *info,
			       struct elf_link_hash_entry *dir,
			       struct elf_link_hash_entry *ind)
{
  c33fdpic_entry (dir)->call |= c33fdpic_entry (ind)->call;
  c33fdpic_entry (dir)->fd |= c33fdpic_entry (ind)->fd;
  _bfd_elf_link_hash_copy_indirect (info, dir, ind);
}

/* .got, .plt, .rela.dyn and .rofixup, which every FDPIC link has, dynamic
   or not.  */

static bool
c33fdpic_create_sections (bfd *abfd, struct bfd_link_info *info)
{
  struct c33fdpic_link_hash_table *htab = c33fdpic_hash_table (info);
  flagword flags = (SEC_ALLOC | SEC_LOAD | SEC_HAS_CONTENTS | SEC_IN_MEMORY
		    | SEC_LINKER_CREATED);
  bfd *dynobj;
  asection *s;

  if (htab->srofixup != NULL)
    return true;
  if (htab->elf.dynobj == NULL)
    htab->elf.dynobj = abfd;
  dynobj = htab->elf.dynobj;

  s = bfd_make_section_anyway_with_flags (dynobj, ".got", flags);
  if (s == NULL || !bfd_set_section_alignment (s, 3))
    return false;
  htab->elf.sgot = s;

  s = bfd_make_section_anyway_with_flags (dynobj, ".plt",
					  flags | SEC_CODE | SEC_READONLY);
  if (s == NULL || !bfd_set_section_alignment (s, 2))
    return false;
  htab->elf.splt = s;

  s = bfd_make_section_anyway_with_flags (dynobj, ".rela.dyn",
					  flags | SEC_READONLY);
  if (s == NULL || !bfd_set_section_alignment (s, 2))
    return false;
  htab->elf.srelgot = s;

  s = bfd_make_section_anyway_with_flags (dynobj, ".rela.plt",
					  flags | SEC_READONLY);
  if (s == NULL || !bfd_set_section_alignment (s, 2))
    return false;
  htab->elf.srelplt = s;

  s = bfd_make_section_anyway_with_flags (dynobj, ".rofixup",
					  flags | SEC_READONLY);
  if (s == NULL || !bfd_set_section_alignment (s, 2))
    return false;
  htab->srofixup = s;
  return true;
}

static bool
c33fdpic_create_dynamic_sections (bfd *abfd, struct bfd_link_info *info)
{
  return c33fdpic_create_sections (abfd, info);
}

/* Per-object state for local symbols: the offset in .got of each one's
   canonical descriptor, C33FDPIC_WANTED before sizing, or C33FDPIC_NONE.  */

static bfd_vma *
c33fdpic_local_fds (bfd *abfd, bool create)
{
  bfd_vma *fds = elf_local_got_offsets (abfd);

  if (fds == NULL && create)
    {
      Elf_Internal_Shdr *symtab_hdr = &elf_tdata (abfd)->symtab_hdr;
      bfd_size_type i, n = symtab_hdr->sh_info;

      fds = bfd_alloc (abfd, (n ? n : 1) * sizeof (bfd_vma));
      if (fds == NULL)
	return NULL;
      for (i = 0; i < n; i++)
	fds[i] = C33FDPIC_NONE;
      elf_local_got_offsets (abfd) = fds;
    }
  return fds;
}

static bool
c33fdpic_call_reloc_p (int r_type)
{
  switch (r_type)
    {
    case R_C33_RH:
    case R_C33_RM:
    case R_C33_RL:
    case R_C33_S_RH:
    case R_C33_S_RM:
    case R_C33_S_RL:
    case R_C33_JP:
      return true;
    default:
      return false;
    }
}

static bool
c33fdpic_tls_reloc_p (int type)
{
  return type >= R_C33_TLS_DTPMOD32 && type <= R_C33_TLS_LE32;
}

static bool
c33fdpic_tls_dynamic_p (struct bfd_link_info *info, int type,
                       struct elf_link_hash_entry *h)
{
  if (type == R_C33_TLS_LE32
      || !elf_hash_table (info)->dynamic_sections_created)
    return false;
  if (type == R_C33_TLS_DTPREL32
      && (h == NULL || SYMBOL_REFERENCES_LOCAL (info, h)))
    return false;
  if (type == R_C33_TLS_TPREL32 && bfd_link_executable (info)
      && (h == NULL || SYMBOL_REFERENCES_LOCAL (info, h)))
    return false;
  return true;
}

static bool
c33fdpic_check_relocs (bfd *abfd, struct bfd_link_info *info,
		       asection *sec, const Elf_Internal_Rela *relocs)
{
  Elf_Internal_Shdr *symtab_hdr;
  struct elf_link_hash_entry **sym_hashes;
  const Elf_Internal_Rela *rel, *rel_end;

  if (bfd_link_relocatable (info))
    return true;
  if (!c33fdpic_create_sections (abfd, info))
    return false;

  symtab_hdr = &elf_tdata (abfd)->symtab_hdr;
  sym_hashes = elf_sym_hashes (abfd);
  rel_end = relocs + sec->reloc_count;
  for (rel = relocs; rel < rel_end; rel++)
    {
      unsigned long r_symndx = ELF32_R_SYM (rel->r_info);
      int r_type = ELF32_R_TYPE (rel->r_info);
      struct elf_link_hash_entry *h = NULL;

      if (r_symndx >= symtab_hdr->sh_info)
	{
	  h = sym_hashes[r_symndx - symtab_hdr->sh_info];
	  while (h->root.type == bfd_link_hash_indirect
		 || h->root.type == bfd_link_hash_warning)
	    h = (struct elf_link_hash_entry *) h->root.u.i.link;
	}

      if (c33fdpic_tls_reloc_p (r_type))
        {
          if (h != NULL && !bfd_elf_link_record_dynamic_symbol (info, h))
            return false;
          if (r_type == R_C33_TLS_TPREL32)
            info->flags |= DF_STATIC_TLS;
        }
      else if (c33fdpic_call_reloc_p (r_type))
	{
	  if (h != NULL)
	    c33fdpic_entry (h)->call = 1;
	}
      else if (r_type == R_C33_FUNCDESC)
	{
	  if (h != NULL)
	    c33fdpic_entry (h)->fd = 1;
	  else
	    {
	      bfd_vma *fds = c33fdpic_local_fds (abfd, true);

	      if (fds == NULL)
		return false;
	      fds[r_symndx] = C33FDPIC_WANTED;
	    }
	}
    }
  return true;
}

/* Symbols defined in a shared library have no output section here.  */

static bool
c33fdpic_defined_p (struct elf_link_hash_entry *h)
{
  return ((h->root.type == bfd_link_hash_defined
	   || h->root.type == bfd_link_hash_defweak)
	  && h->root.u.def.section->output_section != NULL);
}

/* Whether this module owns H's canonical descriptor.  An exported function
   may have its address taken in other modules too, and all of them must
   agree, so ld.so makes those, even under -Bsymbolic: Xt, for one,
   compares a class's procedures with its own _XtInherit's descriptor.  */

static bool
c33fdpic_fd_local (struct bfd_link_info *info, struct elf_link_hash_entry *h)
{
  return (h->dynindx == -1
	  || !elf_hash_table (info)->dynamic_sections_created);
}

/* What the loader has to do for a data word relocated by R_TYPE against H,
   or against a local symbol in SYM_SEC.  relocate_section and the sizing
   below must agree exactly.  */

static enum c33fdpic_word
c33fdpic_word_kind (struct bfd_link_info *info, int r_type,
		    struct elf_link_hash_entry *h, asection *sym_sec)
{
  enum c33fdpic_word own = (bfd_link_executable (info)
			    ? C33FDPIC_WORD_ROFIXUP : C33FDPIC_WORD_RELATIVE);

  if (h == NULL)
    {
      if (r_type == R_C33_32 && sym_sec != NULL && bfd_is_abs_section (sym_sec))
	return C33FDPIC_WORD_PLAIN;
      return own;
    }

  if (h->root.type == bfd_link_hash_undefweak
      && (h->dynindx == -1 || !elf_hash_table (info)->dynamic_sections_created))
    return C33FDPIC_WORD_PLAIN;

  if (r_type == R_C33_FUNCDESC)
    {
      if (!c33fdpic_fd_local (info, h))
	return C33FDPIC_WORD_SYMBOLIC;
      return c33fdpic_defined_p (h) ? own : C33FDPIC_WORD_PLAIN;
    }

  if (!SYMBOL_REFERENCES_LOCAL (info, h))
    return C33FDPIC_WORD_SYMBOLIC;
  if (c33fdpic_defined_p (h) && bfd_is_abs_section (h->root.u.def.section))
    return C33FDPIC_WORD_PLAIN;
  return own;
}

/* Give each symbol its .plt entry and descriptors.  */

static bool
c33fdpic_allocate_global (struct elf_link_hash_entry *h, void *inf)
{
  struct bfd_link_info *info = (struct bfd_link_info *) inf;
  struct c33fdpic_link_hash_table *htab = c33fdpic_hash_table (info);
  struct c33fdpic_link_hash_entry *e = c33fdpic_entry (h);

  if (h->root.type == bfd_link_hash_indirect)
    return true;

  e->plt_offset = e->plt_got = e->fd_got = C33FDPIC_NONE;
  if (e->call
      && htab->elf.dynamic_sections_created
      && h->dynindx != -1
      && !SYMBOL_CALLS_LOCAL (info, h))
    {
      if (htab->elf.splt->size == 0)
	htab->elf.splt->size = C33FDPIC_PLT_HEADER_SIZE;
      e->plt_offset = htab->elf.splt->size;
      htab->elf.splt->size += C33FDPIC_PLT_ENTRY_SIZE;
      e->plt_got = htab->elf.sgot->size;
      htab->elf.sgot->size += C33FDPIC_FD_SIZE;
    }
  if (e->fd && c33fdpic_fd_local (info, h) && c33fdpic_defined_p (h))
    {
      e->fd_got = htab->elf.sgot->size;
      htab->elf.sgot->size += C33FDPIC_FD_SIZE;
    }
  return true;
}

static bool
c33fdpic_c33_input_p (bfd *ibfd)
{
  return (bfd_get_flavour (ibfd) == bfd_target_elf_flavour
	  && elf_elfheader (ibfd)->e_machine == EM_SE_C33
	  && (ibfd->flags & DYNAMIC) == 0);
}

struct c33fdpic_counts
{
  bfd_vma rofixups;
  bfd_vma relas;
};

static bool
c33fdpic_count_global (struct elf_link_hash_entry *h, void *inf)
{
  struct bfd_link_info *info = ((void **) inf)[0];
  struct c33fdpic_counts *n = ((void **) inf)[1];
  struct c33fdpic_link_hash_entry *e = c33fdpic_entry (h);

  if (h->root.type == bfd_link_hash_indirect)
    return true;
  if (e->fd_got != C33FDPIC_NONE)
    {
      if (bfd_link_executable (info))
	n->rofixups += 2;
      else
	n->relas++;
    }
  return true;
}

/* Count the load-time work: the descriptors in .got, and every data word
   that holds an address.  Set the sizes of .rofixup and .rela.dyn from it,
   and say whether they changed.  */

static bool
c33fdpic_size_fixups (struct bfd_link_info *info, bool *changed)
{
  struct c33fdpic_link_hash_table *htab = c33fdpic_hash_table (info);
  struct c33fdpic_counts n = { 1, 0 };	/* __dp is the last fixup.  */
  void *args[2] = { info, &n };
  bfd *ibfd;
  bfd_size_type size;

  elf_link_hash_traverse (&htab->elf, c33fdpic_count_global, args);

  for (ibfd = info->input_bfds; ibfd != NULL; ibfd = ibfd->link.next)
    {
      Elf_Internal_Shdr *symtab_hdr;
      Elf_Internal_Sym *local_syms = NULL;
      struct elf_link_hash_entry **sym_hashes;
      bfd_vma *fds;
      asection *s;

      if (!c33fdpic_c33_input_p (ibfd))
	continue;
      symtab_hdr = &elf_tdata (ibfd)->symtab_hdr;
      sym_hashes = elf_sym_hashes (ibfd);

      fds = c33fdpic_local_fds (ibfd, false);
      if (fds != NULL)
	{
	  bfd_size_type i;

	  for (i = 0; i < symtab_hdr->sh_info; i++)
	    if (fds[i] != C33FDPIC_NONE)
	      {
		if (bfd_link_executable (info))
		  n.rofixups += 2;
		else
		  n.relas++;
	      }
	}

      for (s = ibfd->sections; s != NULL; s = s->next)
	{
	  Elf_Internal_Rela *relocs, *rel, *rel_end;

	  if ((s->flags & (SEC_ALLOC | SEC_RELOC)) != (SEC_ALLOC | SEC_RELOC)
	      || s->reloc_count == 0
	      || (s->flags & SEC_EXCLUDE) != 0
	      || s->output_section == NULL
	      || bfd_is_abs_section (s->output_section)
	      || discarded_section (s))
	    continue;

	  relocs = _bfd_elf_link_read_relocs (ibfd, s, NULL, NULL,
					      info->keep_memory);
	  if (relocs == NULL)
	    return false;

	  rel_end = relocs + s->reloc_count;
	  for (rel = relocs; rel < rel_end; rel++)
	    {
	      int r_type = ELF32_R_TYPE (rel->r_info);
	      unsigned long r_symndx = ELF32_R_SYM (rel->r_info);
	      struct elf_link_hash_entry *h = NULL;
	      asection *sym_sec = NULL;
	      bfd_vma off;

	      if (r_type != R_C33_32 && r_type != R_C33_FUNCDESC
                  && !c33fdpic_tls_reloc_p (r_type))
		continue;
	      off = _bfd_elf_section_offset (info->output_bfd, info, s,
					     rel->r_offset);
	      if (off == (bfd_vma) -1 || off == (bfd_vma) -2)
		continue;

	      if (r_symndx >= symtab_hdr->sh_info)
		{
		  h = sym_hashes[r_symndx - symtab_hdr->sh_info];
		  while (h->root.type == bfd_link_hash_indirect
			 || h->root.type == bfd_link_hash_warning)
		    h = (struct elf_link_hash_entry *) h->root.u.i.link;
		  if ((h->root.type == bfd_link_hash_defined
		       || h->root.type == bfd_link_hash_defweak)
		      && discarded_section (h->root.u.def.section))
		    continue;
		}
	      else
		{
		  if (local_syms == NULL)
		    {
		      local_syms = bfd_elf_get_elf_syms (ibfd, symtab_hdr,
							 symtab_hdr->sh_info,
							 0, NULL, NULL, NULL);
		      if (local_syms == NULL)
			return false;
		    }
		  sym_sec = bfd_section_from_elf_index
		    (ibfd, local_syms[r_symndx].st_shndx);
		  if (sym_sec != NULL && discarded_section (sym_sec))
		    continue;
		}

	      if (c33fdpic_tls_reloc_p (r_type))
        {
          if (c33fdpic_tls_dynamic_p (info, r_type, h))
            n.relas++;
          continue;
        }

      switch (c33fdpic_word_kind (info, r_type, h, sym_sec))
		{
		case C33FDPIC_WORD_PLAIN:
		  break;
		case C33FDPIC_WORD_ROFIXUP:
		  n.rofixups++;
		  break;
		case C33FDPIC_WORD_RELATIVE:
		case C33FDPIC_WORD_SYMBOLIC:
		  n.relas++;
		  break;
		}
	    }

	  if (elf_section_data (s)->relocs != relocs)
	    free (relocs);
	}
      free (local_syms);
    }

  *changed = false;
  size = n.rofixups * 4;
  if (htab->srofixup->size != size)
    {
      htab->srofixup->size = size;
      htab->srofixup->contents = bfd_zalloc (htab->elf.dynobj, size);
      if (htab->srofixup->contents == NULL)
	return false;
      htab->srofixup->alloced = 1;
      *changed = true;
    }
  size = n.relas * sizeof (Elf32_External_Rela);
  if (htab->elf.srelgot->size != size)
    {
      htab->elf.srelgot->size = size;
      htab->elf.srelgot->contents = (size == 0 ? NULL
				     : bfd_zalloc (htab->elf.dynobj, size));
      if (size != 0 && htab->elf.srelgot->contents == NULL)
	return false;
      htab->elf.srelgot->alloced = 1;
      *changed = true;
    }
  return true;
}

static bool
c33fdpic_late_size_sections (struct bfd_link_info *info)
{
  struct c33fdpic_link_hash_table *htab = c33fdpic_hash_table (info);
  bfd *dynobj = htab->elf.dynobj;
  bfd *ibfd;
  bool changed;
  asection *s;

  /* Keep the linker-generated header table in link-time address space.
     The C33 FDPIC unwinder maps the PC before searching and the selected
     FDE afterwards; the table itself needs no runtime relocations. */

  if (dynobj == NULL || htab->srofixup == NULL)
    return true;

  /* The descriptors' relocations are written after the final link sorts
     .rela.dyn, so it must not be sorted then:
     c33fdpic_finish_dynamic_sections puts the module-relative ones first
     itself.  */
  info->combreloc = false;

  if (htab->elf.dynamic_sections_created
      && bfd_link_executable (info)
      && !info->nointerp)
    {
      s = bfd_get_linker_section (dynobj, ".interp");
      BFD_ASSERT (s != NULL);
      s->size = sizeof ELF_DYNAMIC_INTERPRETER;
      s->contents = (unsigned char *) ELF_DYNAMIC_INTERPRETER;
      s->alloced = 1;
    }

  htab->elf.sgot->size = (htab->elf.dynamic_sections_created
			  ? C33FDPIC_GOT_HEADER_SIZE : 0);
  htab->elf.splt->size = 0;
  elf_link_hash_traverse (&htab->elf, c33fdpic_allocate_global, info);
  for (ibfd = info->input_bfds; ibfd != NULL; ibfd = ibfd->link.next)
    {
      bfd_vma *fds;
      bfd_size_type i;

      if (!c33fdpic_c33_input_p (ibfd)
	  || (fds = c33fdpic_local_fds (ibfd, false)) == NULL)
	continue;
      for (i = 0; i < elf_tdata (ibfd)->symtab_hdr.sh_info; i++)
	if (fds[i] != C33FDPIC_NONE)
	  {
	    fds[i] = htab->elf.sgot->size;
	    htab->elf.sgot->size += C33FDPIC_FD_SIZE;
	  }
    }

  htab->elf.srelplt->size = 0;
  if (htab->elf.splt->size != 0)
    htab->elf.srelplt->size
      = ((htab->elf.splt->size - C33FDPIC_PLT_HEADER_SIZE)
	 / C33FDPIC_PLT_ENTRY_SIZE * sizeof (Elf32_External_Rela));

  for (s = htab->elf.sgot; s != NULL;
       s = (s == htab->elf.sgot ? htab->elf.splt
	    : s == htab->elf.splt ? htab->elf.srelplt : NULL))
    if (s->size != 0)
      {
	s->contents = bfd_zalloc (dynobj, s->size);
	if (s->contents == NULL)
	  return false;
	s->alloced = 1;
      }
    else
      s->flags |= SEC_EXCLUDE;

  if (!c33fdpic_size_fixups (info, &changed))
    return false;

  if (htab->elf.dynamic_sections_created)
    {
      if (bfd_link_executable (info)
	  && !_bfd_elf_add_dynamic_entry (info, DT_DEBUG, 0))
	return false;
      if (!_bfd_elf_add_dynamic_entry (info, DT_PLTGOT, 0)
	  || !_bfd_elf_add_dynamic_entry (info, DT_RELA, 0)
	  || !_bfd_elf_add_dynamic_entry (info, DT_RELASZ, 0)
	  || !_bfd_elf_add_dynamic_entry (info, DT_RELAENT,
					  sizeof (Elf32_External_Rela))
	  || !_bfd_elf_add_dynamic_entry (info, DT_RELACOUNT, 0))
	return false;
      if (htab->elf.srelplt->size != 0
	  && (!_bfd_elf_add_dynamic_entry (info, DT_JMPREL, 0)
	      || !_bfd_elf_add_dynamic_entry (info, DT_PLTRELSZ, 0)
	      || !_bfd_elf_add_dynamic_entry (info, DT_PLTREL, DT_RELA)))
	return false;
    }
  return true;
}

/* .eh_frame has been edited, and a dropped entry drops its fixup.  */

static bool
c33fdpic_discard_info (bfd *ibfd ATTRIBUTE_UNUSED,
		       struct elf_reloc_cookie *cookie ATTRIBUTE_UNUSED,
		       struct bfd_link_info *info)
{
  struct c33fdpic_link_hash_table *htab = c33fdpic_hash_table (info);
  bool changed = false;

  if (!IS_FDPIC (info->output_bfd)
      || htab->elf.dynobj == NULL
      || htab->srofixup == NULL
      || htab->recounted)
    return false;
  htab->recounted = true;
  if (!c33fdpic_size_fixups (info, &changed))
    info->callbacks->einfo (_("%F%P: cannot size .rofixup: %E\n"));
  return changed;
}

static void
c33fdpic_add_rofixup (bfd *output_bfd, struct c33fdpic_link_hash_table *htab,
		      bfd_vma address)
{
  asection *s = htab->srofixup;

  if ((htab->rofixup_count + 1) * 4 <= s->size)
    bfd_put_32 (output_bfd, address, s->contents + htab->rofixup_count * 4);
  htab->rofixup_count++;
}

static void
c33fdpic_add_rela_to (bfd *output_bfd, asection *s, bfd_vma *count,
		      bfd_vma offset, int r_type, long dynindx, bfd_vma addend)
{
  Elf_Internal_Rela rela;

  rela.r_offset = offset;
  rela.r_info = ELF32_R_INFO (dynindx, r_type);
  rela.r_addend = addend;
  if ((*count + 1) * sizeof (Elf32_External_Rela) <= s->size)
    bfd_elf32_swap_reloca_out (output_bfd, &rela,
			       s->contents
			       + *count * sizeof (Elf32_External_Rela));
  ++*count;
}

static void
c33fdpic_add_rela (bfd *output_bfd, struct c33fdpic_link_hash_table *htab,
		   bfd_vma offset, int r_type, long dynindx, bfd_vma addend)
{
  c33fdpic_add_rela_to (output_bfd, htab->elf.srelgot, &htab->rela_count,
			offset, r_type, dynindx, addend);
}

/* Move the module-relative relocations, R_C33_RELATIVE, to
   the front of .rela.dyn, keeping the order within each kind, and return
   how many there are: DT_RELACOUNT.  ld.so runs them through a loop of
   their own, and they are most of a shared library's.  */

static bfd_vma
c33fdpic_sort_relas (bfd *output_bfd, asection *s)
{
  bfd_size_type n = s->size / sizeof (Elf32_External_Rela), i;
  bfd_vma relative = 0, other;
  bfd_byte *copy;

  if (n == 0)
    return 0;
  copy = bfd_malloc (s->size);
  if (copy == NULL)
    return (bfd_vma) -1;
  memcpy (copy, s->contents, s->size);
  for (i = 0; i < n; i++)
    {
      Elf_Internal_Rela rela;

      bfd_elf32_swap_reloca_in (output_bfd,
				copy + i * sizeof (Elf32_External_Rela),
				&rela);
      if (ELF32_R_TYPE (rela.r_info) == R_C33_RELATIVE)
	relative++;
    }
  other = relative;
  relative = 0;
  for (i = 0; i < n; i++)
    {
      bfd_byte *from = copy + i * sizeof (Elf32_External_Rela);
      Elf_Internal_Rela rela;
      bfd_vma *to;

      bfd_elf32_swap_reloca_in (output_bfd, from, &rela);
      to = ELF32_R_TYPE (rela.r_info) == R_C33_RELATIVE ? &relative : &other;
      memcpy (s->contents + *to * sizeof (Elf32_External_Rela), from,
	      sizeof (Elf32_External_Rela));
      ++*to;
    }
  free (copy);
  return relative;
}

static bool
c33fdpic_osec_readonly_p (bfd *output_bfd, asection *osec)
{
  Elf_Internal_Phdr *p
    = _bfd_elf_find_segment_containing_section (output_bfd, osec);

  return p != NULL && (p->p_flags & PF_W) == 0;
}

static bfd_vma
c33fdpic_dp (struct bfd_link_info *info)
{
  struct elf_link_hash_entry *h
    = elf_link_hash_lookup (elf_hash_table (info), "__dp", false, false, true);

  if (h == NULL || !c33fdpic_defined_p (h))
    return 0;
  return (h->root.u.def.value
	  + h->root.u.def.section->output_section->vma
	  + h->root.u.def.section->output_offset);
}

static bfd_vma
c33fdpic_got_vma (struct c33fdpic_link_hash_table *htab)
{
  return htab->elf.sgot->output_section->vma + htab->elf.sgot->output_offset;
}

/* Write a descriptor for ENTRY at OFFSET in .got, and what loads it.  */

static void
c33fdpic_write_fd (struct bfd_link_info *info, bfd_vma offset, bfd_vma entry)
{
  struct c33fdpic_link_hash_table *htab = c33fdpic_hash_table (info);
  bfd *output_bfd = info->output_bfd;
  bfd_vma where = c33fdpic_got_vma (htab) + offset;

  bfd_put_32 (output_bfd, entry, htab->elf.sgot->contents + offset);
  bfd_put_32 (output_bfd, c33fdpic_dp (info),
	      htab->elf.sgot->contents + offset + 4);
  if (bfd_link_executable (info))
    {
      c33fdpic_add_rofixup (output_bfd, htab, where);
      c33fdpic_add_rofixup (output_bfd, htab, where + 4);
    }
  else
    c33fdpic_add_rela (output_bfd, htab, where, R_C33_FUNCDESC_VALUE, 0, entry);
}

/* A .plt entry: the descriptor at DOFF from %r15, which is still the
   caller's, then its %r15 and a jump to its entry point.  %r14 is not an
   argument register and calls clobber it.

	ext	doff_hi(slot)
	ext	doff_lo(slot)
	ld.w	%r14,[%r15]
	ext	doff_hi(slot+4)
	ext	doff_lo(slot+4)
	ld.w	%r15,[%r15]
	jp	%r14
	nop  */

static void
c33fdpic_write_plt (bfd *output_bfd, bfd_byte *p, bfd_vma doff)
{
  bfd_put_16 (output_bfd, 0xc000 | ((doff >> 13) & 0x1fff), p);
  bfd_put_16 (output_bfd, 0xc000 | (doff & 0x1fff), p + 2);
  bfd_put_16 (output_bfd, 0x30fe, p + 4);
  doff += 4;
  bfd_put_16 (output_bfd, 0xc000 | ((doff >> 13) & 0x1fff), p + 6);
  bfd_put_16 (output_bfd, 0xc000 | (doff & 0x1fff), p + 8);
  bfd_put_16 (output_bfd, 0x30ff, p + 10);
  bfd_put_16 (output_bfd, 0x068e, p + 12);
  bfd_put_16 (output_bfd, 0x0000, p + 14);
}

/* The stub an unresolved descriptor points at: the entry's offset in
   .rela.plt in %r13, then a call to the trampoline at the head of .plt.
   The call is not for returning: its return address tells the resolver
   which module's .plt this is, so that it needs nothing from %r15, which a
   caller reading the descriptor while another thread or a signal handler
   resolves it may have taken from the new one.  %r13 carries no argument.

	xld.w	%r13,RELOC_OFFSET
	xcall	.plt  */

static void
c33fdpic_write_plt_lazy (bfd *output_bfd, bfd_byte *p, bfd_vma reloc_offset,
			 bfd_signed_vma disp)
{
  bfd_put_16 (output_bfd, 0xc000 | ((reloc_offset >> 19) & 0x1fff), p);
  bfd_put_16 (output_bfd, 0xc000 | ((reloc_offset >> 6) & 0x1fff), p + 2);
  bfd_put_16 (output_bfd, 0x6c0d | ((reloc_offset & 0x3f) << 4), p + 4);
  /* DISP is from the call instruction itself.  */
  bfd_put_16 (output_bfd, 0xc000 | ((disp >> 19) & 0x1ff8), p + 6);
  bfd_put_16 (output_bfd, 0xc000 | ((disp >> 9) & 0x1fff), p + 8);
  bfd_put_16 (output_bfd, 0x1c00 | ((disp >> 1) & 0xff), p + 10);
}

/* The head of .plt: into ld.so's resolver, whose descriptor every module's
   .got header holds (ld.so's INIT_GOT), through whichever module's %r15
   the caller has.

	ld.w	%r14,[%r15]
	xld.w	%r15,[%r15+4]
	jp	%r14  */

static void
c33fdpic_write_plt_header (bfd *output_bfd, bfd_byte *p)
{
  bfd_put_16 (output_bfd, 0x30fe, p);
  bfd_put_16 (output_bfd, 0xc004, p + 2);
  bfd_put_16 (output_bfd, 0x30ff, p + 4);
  bfd_put_16 (output_bfd, 0x068e, p + 6);
}

static bool
c33fdpic_finish_global (struct elf_link_hash_entry *h, void *inf)
{
  struct bfd_link_info *info = (struct bfd_link_info *) inf;
  struct c33fdpic_link_hash_table *htab = c33fdpic_hash_table (info);
  struct c33fdpic_link_hash_entry *e = c33fdpic_entry (h);
  bfd *output_bfd = info->output_bfd;

  if (h->root.type == bfd_link_hash_indirect)
    return true;

  if (e->plt_offset != C33FDPIC_NONE)
    {
      bfd_vma slot = c33fdpic_got_vma (htab) + e->plt_got;
      bfd_vma plt = (htab->elf.splt->output_section->vma
		     + htab->elf.splt->output_offset);
      bfd_vma lazy = plt + e->plt_offset + C33FDPIC_PLT_LAZY_OFFSET;
      bfd_vma index = ((e->plt_offset - C33FDPIC_PLT_HEADER_SIZE)
		       / C33FDPIC_PLT_ENTRY_SIZE);
      bfd_vma relplt_index = index;

      c33fdpic_write_plt (output_bfd, htab->elf.splt->contents + e->plt_offset,
			  slot - c33fdpic_dp (info));
      c33fdpic_write_plt_lazy (output_bfd,
			       htab->elf.splt->contents + e->plt_offset
			       + C33FDPIC_PLT_LAZY_OFFSET,
			       index * sizeof (Elf32_External_Rela),
			       (bfd_signed_vma) plt - (bfd_signed_vma) (lazy + 10));
      /* Until ld.so resolves it, the descriptor sends a call to the stub;
	 ld.so relocates the link-time address here and adds %r15.  */
      bfd_put_32 (output_bfd, lazy, htab->elf.sgot->contents + e->plt_got);
      bfd_put_32 (output_bfd, 0, htab->elf.sgot->contents + e->plt_got + 4);
      c33fdpic_add_rela_to (output_bfd, htab->elf.srelplt, &relplt_index,
			    slot, R_C33_FUNCDESC_VALUE, h->dynindx, 0);
      htab->relplt_count++;
    }
  if (e->fd_got != C33FDPIC_NONE)
    c33fdpic_write_fd (info, e->fd_got,
		       h->root.u.def.value
		       + h->root.u.def.section->output_section->vma
		       + h->root.u.def.section->output_offset);
  return true;
}

static bool
c33fdpic_finish_dynamic_sections (struct bfd_link_info *info,
				  bfd_byte *buf ATTRIBUTE_UNUSED)
{
  struct c33fdpic_link_hash_table *htab = c33fdpic_hash_table (info);
  bfd *output_bfd = info->output_bfd;
  bfd *ibfd;
  bfd_vma relative;

  if (htab->srofixup == NULL)
    return true;

  elf_link_hash_traverse (&htab->elf, c33fdpic_finish_global, info);

  for (ibfd = info->input_bfds; ibfd != NULL; ibfd = ibfd->link.next)
    {
      Elf_Internal_Shdr *symtab_hdr;
      Elf_Internal_Sym *local_syms;
      bfd_vma *fds;
      bfd_size_type i;

      if (!c33fdpic_c33_input_p (ibfd)
	  || (fds = c33fdpic_local_fds (ibfd, false)) == NULL)
	continue;
      symtab_hdr = &elf_tdata (ibfd)->symtab_hdr;
      local_syms = bfd_elf_get_elf_syms (ibfd, symtab_hdr, symtab_hdr->sh_info,
					 0, NULL, NULL, NULL);
      if (local_syms == NULL)
	return false;
      for (i = 0; i < symtab_hdr->sh_info; i++)
	if (fds[i] != C33FDPIC_NONE)
	  {
	    asection *sec = bfd_section_from_elf_index (ibfd,
							local_syms[i].st_shndx);
	    bfd_vma entry = local_syms[i].st_value;

	    if (sec != NULL && sec->output_section != NULL)
	      entry += sec->output_section->vma + sec->output_offset;
	    c33fdpic_write_fd (info, fds[i], entry);
	  }
      free (local_syms);
    }

  /* The last fixup: __self_reloc returns it, relocated, as %r15.  */
  c33fdpic_add_rofixup (output_bfd, htab, c33fdpic_dp (info));

  if (htab->elf.splt->size != 0)
    c33fdpic_write_plt_header (output_bfd, htab->elf.splt->contents);

  if (htab->rofixup_count * 4 != htab->srofixup->size
      || htab->rela_count * sizeof (Elf32_External_Rela)
	 != htab->elf.srelgot->size
      || htab->relplt_count * sizeof (Elf32_External_Rela)
	 != htab->elf.srelplt->size)
    {
      _bfd_error_handler
	(_("%pB: internal error: %" PRIu64 " of %" PRIu64 " .rofixup entries"
	   " and %" PRIu64 " of %" PRIu64 " dynamic relocations written"),
	 output_bfd, (uint64_t) htab->rofixup_count,
	 (uint64_t) (htab->srofixup->size / 4),
	 (uint64_t) htab->rela_count,
	 (uint64_t) (htab->elf.srelgot->size / sizeof (Elf32_External_Rela)));
      return false;
    }

  relative = c33fdpic_sort_relas (output_bfd, htab->elf.srelgot);
  if (relative == (bfd_vma) -1)
    return false;

  if (htab->elf.dynamic_sections_created)
    {
      asection *sdyn = bfd_get_linker_section (htab->elf.dynobj, ".dynamic");
      Elf32_External_Dyn *dyncon, *dynconend;

      BFD_ASSERT (sdyn != NULL);
      dyncon = (Elf32_External_Dyn *) sdyn->contents;
      dynconend = (Elf32_External_Dyn *) (sdyn->contents + sdyn->size);
      for (; dyncon < dynconend; dyncon++)
	{
	  Elf_Internal_Dyn dyn;

	  asection *o;

	  bfd_elf32_swap_dyn_in (htab->elf.dynobj, dyncon, &dyn);
	  switch (dyn.d_tag)
	    {
	    case DT_PLTGOT:
	      dyn.d_un.d_ptr = c33fdpic_dp (info);
	      break;
	    case DT_RELACOUNT:
	      dyn.d_un.d_val = relative;
	      break;
	    /* The generic code counts every RELA section into DT_RELA; the
	       .plt's are a table of their own.  */
	    case DT_RELA:
	    case DT_RELASZ:
	    case DT_JMPREL:
	    case DT_PLTRELSZ:
	      o = (dyn.d_tag == DT_RELA || dyn.d_tag == DT_RELASZ
		   ? htab->elf.srelgot : htab->elf.srelplt);
	      if (dyn.d_tag == DT_RELASZ || dyn.d_tag == DT_PLTRELSZ)
		dyn.d_un.d_val = o->size;
	      else
		dyn.d_un.d_ptr = (o->output_section->vma + o->output_offset);
	      break;
	    default:
	      continue;
	    }
	  bfd_elf32_swap_dyn_out (output_bfd, &dyn, dyncon);
	}
    }
  return true;
}

static bool
c33fdpic_adjust_dynamic_symbol (struct bfd_link_info *info ATTRIBUTE_UNUSED,
				struct elf_link_hash_entry *h ATTRIBUTE_UNUSED)
{
  /* No copy relocations: a module reaches another's data through a word
     holding its address.  */
  return true;
}

static bool
c33fdpic_finish_dynamic_symbol (struct bfd_link_info *info ATTRIBUTE_UNUSED,
				struct elf_link_hash_entry *h ATTRIBUTE_UNUSED,
				Elf_Internal_Sym *sym ATTRIBUTE_UNUSED)
{
  return true;
}

/* The FDPIC part of relocating one relocation.  Returns 1 when it has
   finished with the relocation, 0 to carry on with RELOCATION, possibly
   changed, and -1 on an error.  */

static int
c33fdpic_relocate (struct bfd_link_info *info, bfd *input_bfd,
		   asection *input_section, bfd_byte *contents,
		   Elf_Internal_Rela *rel, int r_type,
		   struct elf_link_hash_entry *h, asection *sec,
		   bfd_vma *relocation, bool *unresolved_reloc)
{
  struct c33fdpic_link_hash_table *htab = c33fdpic_hash_table (info);
  bfd *output_bfd = info->output_bfd;
  unsigned long r_symndx = ELF32_R_SYM (rel->r_info);
  const char *name = h != NULL ? h->root.root.string : NULL;
  enum c33fdpic_word kind;
  bfd_vma offset, where, value;

  if (c33fdpic_tls_reloc_p (r_type))
    {
      bool dynamic = c33fdpic_tls_dynamic_p (info, r_type, h);
      bool local = h == NULL || SYMBOL_REFERENCES_LOCAL (info, h);
      asection *tls_sec = elf_hash_table (info)->tls_sec;
      bfd_vma tls_offset = 0;
      bfd_vma symndx = 0;

      if (r_type == R_C33_TLS_DTPMOD32 && r_symndx == 0)
        /* The local-dynamic module base has no associated TLS symbol. */
        tls_offset = 0;
      else if (local)
        {
          if (tls_sec == NULL || sec == NULL || !(sec->flags & SEC_THREAD_LOCAL))
            {
              _bfd_error_handler (_("%pB: TLS relocation against a non-TLS symbol"),
                                  input_bfd);
              return -1;
            }
          tls_offset = *relocation - tls_sec->vma;
        }
      else if (dynamic)
        symndx = h->dynindx;
      else
        {
          _bfd_error_handler (_("%pB: local-exec TLS needs a locally defined symbol"),
                              input_bfd);
          return -1;
        }

      if (r_type == R_C33_TLS_LE32 && !bfd_link_executable (info))
        {
          _bfd_error_handler (_("%pB: local-exec TLS is invalid in a shared library"),
                              input_bfd);
          return -1;
        }
      offset = _bfd_elf_section_offset (output_bfd, info, input_section,
                                       rel->r_offset);
      if (offset == (bfd_vma)-1 || offset == (bfd_vma)-2)
        return 1;
      where = input_section->output_section->vma + input_section->output_offset
              + offset;
      value = tls_offset + rel->r_addend;
      if (dynamic)
        {
          if (input_section->output_section->flags & SEC_READONLY)
            {
              _bfd_error_handler (_("%pB: dynamic TLS relocation in read-only data"),
                                  input_bfd);
              return -1;
            }
          if (r_type == R_C33_TLS_DTPMOD32)
            value = 0;
          c33fdpic_add_rela (output_bfd, htab, where, r_type, symndx, value);
          value = 0;
        }
      else if (r_type == R_C33_TLS_DTPMOD32)
        value = 1;
      else if (r_type == R_C33_TLS_TPREL32 || r_type == R_C33_TLS_LE32)
        /* Static variant I includes TCB alignment padding. */
        value += (8 + (1ul << elf_hash_table (info)->tls_sec->alignment_power) - 1)
                 & ~((1ul << elf_hash_table (info)->tls_sec->alignment_power) - 1);
      bfd_put_32 (output_bfd, value, contents + rel->r_offset);
      *unresolved_reloc = false;
      return 1;
    }

  if (c33fdpic_call_reloc_p (r_type))
    {
      if (h != NULL && c33fdpic_entry (h)->plt_offset != C33FDPIC_NONE)
	{
	  *relocation = (htab->elf.splt->output_section->vma
			 + htab->elf.splt->output_offset
			 + c33fdpic_entry (h)->plt_offset);
	  *unresolved_reloc = false;
	}
      else if (h != NULL
	       && (h->root.type == bfd_link_hash_defined
		   || h->root.type == bfd_link_hash_defweak)
	       && !c33fdpic_defined_p (h))
	{
	  _bfd_error_handler
	    (_("%pB(%pA+%#" PRIx64 "): call to %s in another module has no"
	       " .plt entry"), input_bfd, input_section,
	     (uint64_t) rel->r_offset, name);
	  return -1;
	}
      return 0;
    }

  switch (r_type)
    {
    case R_C33_DH:
    case R_C33_DL:
    case R_C33_DPH:
    case R_C33_DPM:
    case R_C33_DPL:
      if (h != NULL
	  && (h->root.type == bfd_link_hash_defined
	      || h->root.type == bfd_link_hash_defweak)
	  && !c33fdpic_defined_p (h))
	{
	  _bfd_error_handler
	    (_("%pB(%pA+%#" PRIx64 "): %s is in another module, so %%r15"
	       " cannot reach it; compile with -mfdpic"), input_bfd,
	     input_section, (uint64_t) rel->r_offset, name);
	  return -1;
	}
      return 0;

    case R_C33_32:
    case R_C33_FUNCDESC:
      break;

    default:
      return 0;
    }

  /* Debugging sections just record the link-time value.  */
  if ((input_section->flags & SEC_ALLOC) == 0)
    {
      if (r_type == R_C33_32)
	return 0;
      bfd_put_32 (input_bfd, *relocation + rel->r_addend,
		  contents + rel->r_offset);
      return 1;
    }

  kind = c33fdpic_word_kind (info, r_type, h,
			     h == NULL ? sec : NULL);
  offset = _bfd_elf_section_offset (output_bfd, info, input_section,
				    rel->r_offset);
  if (offset == (bfd_vma) -1 || offset == (bfd_vma) -2)
    return 1;
  where = (input_section->output_section->vma + input_section->output_offset
	   + offset);

  if (r_type == R_C33_32)
    value = *relocation + rel->r_addend;
  else if (kind == C33FDPIC_WORD_SYMBOLIC || kind == C33FDPIC_WORD_PLAIN)
    value = 0;
  else
    {
      bfd_vma fd;

      if (h != NULL)
	fd = c33fdpic_entry (h)->fd_got;
      else
	{
	  bfd_vma *fds = c33fdpic_local_fds (input_bfd, false);
	  fd = fds != NULL ? fds[r_symndx] : C33FDPIC_NONE;
	}
      if (fd == C33FDPIC_NONE || fd == C33FDPIC_WANTED || rel->r_addend != 0)
	{
	  _bfd_error_handler
	    (_("%pB(%pA+%#" PRIx64 "): internal error: no descriptor"),
	     input_bfd, input_section, (uint64_t) rel->r_offset);
	  return -1;
	}
      value = c33fdpic_got_vma (htab) + fd;
    }

  if (kind != C33FDPIC_WORD_PLAIN
      && c33fdpic_osec_readonly_p (output_bfd, input_section->output_section))
    {
      _bfd_error_handler
	(_("%pB(%pA+%#" PRIx64 "): an address in a read-only segment;"
	   " the loader cannot relocate it"), input_bfd, input_section,
	 (uint64_t) rel->r_offset);
      return -1;
    }

  switch (kind)
    {
    case C33FDPIC_WORD_PLAIN:
      break;
    case C33FDPIC_WORD_ROFIXUP:
      c33fdpic_add_rofixup (output_bfd, htab, where);
      break;
    case C33FDPIC_WORD_RELATIVE:
      c33fdpic_add_rela (output_bfd, htab, where, R_C33_RELATIVE, 0, value);
      break;
    case C33FDPIC_WORD_SYMBOLIC:
      value = r_type == R_C33_32 ? rel->r_addend : 0;
      c33fdpic_add_rela (output_bfd, htab, where, r_type, h->dynindx, value);
      break;
    }
  bfd_put_32 (input_bfd, value, contents + rel->r_offset);
  return 1;
}

/* Objects built for FDPIC carry EF_C33_FDPIC, and only the FDPIC vector
   takes them, so the two cannot be mixed by accident.  */

static bool
c33_elf_object_p (bfd *abfd)
{
  return (((elf_elfheader (abfd)->e_flags & EF_C33_FDPIC) != 0)
	  == IS_FDPIC (abfd));
}

static bool
c33fdpic_final_write_processing (bfd *abfd)
{
  elf_elfheader (abfd)->e_flags |= EF_C33_FDPIC;
  return _bfd_elf_final_write_processing (abfd);
}

/* Relocate an C33 ELF section.  */
static int
c33_elf_relocate_section (struct bfd_link_info * info,
			  bfd *                  input_bfd,
			  asection *             input_section,
			  bfd_byte *             contents,
			  Elf_Internal_Rela *    relocs,
			  Elf_Internal_Sym *     local_syms,
			  asection **            local_sections)
{
  /* The backend hook no longer receives the output bfd directly.  */
  bfd * output_bfd = info->output_bfd;
  Elf_Internal_Shdr *           symtab_hdr;
  struct elf_link_hash_entry ** sym_hashes;
  Elf_Internal_Rela *           rel;
  Elf_Internal_Rela *           relend;

  symtab_hdr = & elf_tdata (input_bfd)->symtab_hdr;
  sym_hashes = elf_sym_hashes (input_bfd);

  /* Reset the list of remembered HI16S relocs to empty.  */
  free_ah     = previous_ah;
  previous_ah = NULL;
  ah_counter  = 0;

  
  rel    = relocs;
  relend = relocs + input_section->reloc_count;
  for (; rel < relend; rel++)
    {
      int                          r_type;
      reloc_howto_type *           howto;
      unsigned long                r_symndx;
      Elf_Internal_Sym *           sym;
      asection *                   sec;
      struct elf_link_hash_entry * h;
      bfd_vma                      relocation;
      bfd_reloc_status_type        r;
      bool                         unresolved_reloc = false;

      r_symndx = ELF32_R_SYM (rel->r_info);
      r_type   = ELF32_R_TYPE (rel->r_info);
      howto = c33_elf_howto_table + r_type;

      if (bfd_link_relocatable (info))
	{
	  /* Relocations against symbols in discarded linkonce/COMDAT
	     sections need the same cleanup as they do during a final link.
	     In particular, debug relocations must be removed and emitted
	     relocations must be changed to R_C33_NONE.  */
	  h = NULL;
	  sym = NULL;
	  sec = NULL;
	  if (r_symndx < symtab_hdr->sh_info)
	    {
	      sym = local_syms + r_symndx;
	      sec = local_sections[r_symndx];
	    }
	  else if (sym_hashes != NULL)
	    {
	      h = sym_hashes[r_symndx - symtab_hdr->sh_info];
	      while (h->root.type == bfd_link_hash_indirect
		     || h->root.type == bfd_link_hash_warning)
		h = (struct elf_link_hash_entry *) h->root.u.i.link;
	      if (h->root.type == bfd_link_hash_defined
		  || h->root.type == bfd_link_hash_defweak)
		sec = h->root.u.def.section;
	    }

	  if (sec != NULL && discarded_section (sec))
	    RELOC_AGAINST_DISCARDED_SECTION (info, input_bfd, input_section,
				       rel, 1, relend, R_C33_NONE,
				       howto, 0, contents);

	  /* This is a relocateable link.  We don't have to change
             anything, unless the reloc is against a section symbol,
             in which case we have to adjust according to where the
             section symbol winds up in the output section.  */
	  if (r_symndx < symtab_hdr->sh_info)
	    {
	      sym = local_syms + r_symndx;
	      if (ELF_ST_TYPE (sym->st_info) == STT_SECTION)
		{
		  sec = local_sections[r_symndx];
		  rel->r_addend += sec->output_offset + sym->st_value;
		}
	    }

	  continue;
	}

      /* This is a final link.  */
      h = NULL;
      sym = NULL;
      sec = NULL;
      if (r_symndx < symtab_hdr->sh_info)
	{
	  sym = local_syms + r_symndx;
	  sec = local_sections[r_symndx];
	  /* This also redirects references into SHF_MERGE sections to the
	     surviving merged string/constant.  Computing output_offset by hand
	     leaves a relocation pointing just past a discarded duplicate.  */
	  relocation = _bfd_elf_rela_local_sym (output_bfd, sym, &sec, rel);
#if 0
	  {
	    char * name;
	    name = bfd_elf_string_from_elf_section (input_bfd, symtab_hdr->sh_link, sym->st_name);
	    name = (name == NULL) ? "<none>" : name;
fprintf (stderr, "local: sec: %s, sym: %s (%d), value: %x + %x + %x addend %x\n",
	 sec->name, name, sym->st_name,
	 sec->output_section->vma, sec->output_offset, sym->st_value, rel->r_addend);
	  }
#endif
	}
      else
	{
	  /* A file may validly contain only local symbols and relocations.
	     Delay this check until a global relocation actually needs the
	     hash table, as the V850 backend does.  */
	  if (sym_hashes == NULL)
	    {
	      info->callbacks->warning
		(info, "no hash table available", NULL, input_bfd,
		 input_section, 0);

	      return false;
	    }

	  h = sym_hashes[r_symndx - symtab_hdr->sh_info];
	  
	  while (h->root.type == bfd_link_hash_indirect
		 || h->root.type == bfd_link_hash_warning)
	    h = (struct elf_link_hash_entry *) h->root.u.i.link;
	  
	  if ((h->root.type == bfd_link_hash_defined
	       || h->root.type == bfd_link_hash_defweak)
	      && h->root.u.def.section->output_section == NULL)
	    {
	      /* Defined in a shared library: FDPIC resolves it at load
		 time, through a .plt entry or a dynamic relocation.  */
	      sec = NULL;
	      relocation = 0;
	    }
	  else if (h->root.type == bfd_link_hash_defined
	      || h->root.type == bfd_link_hash_defweak)
	    {
	      sec = h->root.u.def.section;
	      relocation = (h->root.u.def.value
			    + sec->output_section->vma
			    + sec->output_offset);
#if 0
fprintf (stderr, "defined: sec: %s, name: %s, value: %x + %x + %x gives: %x\n",
	 sec->name, h->root.root.string, h->root.u.def.value, sec->output_section->vma, sec->output_offset, relocation);
#endif
	    }
	  else if (h->root.type == bfd_link_hash_undefweak)
	    {
#if 0
fprintf (stderr, "undefined: sec: %s, name: %s\n",
	 sec->name, h->root.root.string);
#endif
	      relocation = 0;

	      /* An undefined weak function has address zero.  Absolute
		 relocations can encode that normally, but a C33 short
		 PC-relative call from the linked image cannot reach address
		 zero.  Such calls are guarded by a zero-address test and are
		 never executed.  Leave their zero placeholder untouched rather
		 than issuing a spurious range diagnostic for dead call code.  */
	      if (howto->pc_relative)
		unresolved_reloc = true;
	    }
	  else if (IS_FDPIC (output_bfd)
		   && info->unresolved_syms_in_objects == RM_IGNORE
		   && ELF_ST_VISIBILITY (h->other) == STV_DEFAULT)
	    /* A shared library may leave it to the dynamic linker.  */
	    relocation = 0;
	  else
	    {
	      (*info->callbacks->undefined_symbol)
		(info, h->root.root.string, input_bfd,
		 input_section, rel->r_offset, true);

	      /* The callback above has already diagnosed the unresolved
		 symbol.  Do not then relocate its placeholder value of zero:
		 for a PC-relative short call that manufactures a huge negative
		 displacement and emits a second, bogus "out of range" warning.
		 Other ELF backends likewise leave an unresolved relocation
		 untouched after reporting it.  */
	      unresolved_reloc = true;
#if 0
fprintf (stderr, "unknown: name: %s\n", h->root.root.string);
#endif
	      relocation = 0;
	    }
	}

      if (sec != NULL && discarded_section (sec))
	RELOC_AGAINST_DISCARDED_SECTION (info, input_bfd, input_section,
					 rel, 1, relend, R_C33_NONE,
					 howto, 0, contents);

      if (IS_FDPIC (output_bfd))
	{
	  int done = c33fdpic_relocate (info, input_bfd, input_section,
					contents, rel, r_type, h, sec,
					&relocation, &unresolved_reloc);

	  if (done < 0)
	    return false;
	  if (done > 0)
	    continue;
	}

      if (unresolved_reloc)
	continue;

      /* FIXME: We should use the addend, but the COFF relocations
         don't.  */
      r = c33_elf_final_link_relocate (howto, input_bfd, output_bfd,
					input_section,
					contents, rel->r_offset,
					relocation, rel->r_addend,
					info, sec, h == NULL);

      if (r != bfd_reloc_ok)
	{
	  const char * name;
	  const char * msg = (const char *)0;

	  if (h != NULL)
	    name = h->root.root.string;
	  else
	    {
	      name = (bfd_elf_string_from_elf_section
		      (input_bfd, symtab_hdr->sh_link, sym->st_name));
	      if (name == NULL || *name == '\0')
			name = bfd_section_name (sec);
	    }

	  switch (r)
	    {
	    case bfd_reloc_overflow:
	    case bfd_reloc_outofrange:
	      /* Both statuses mean the requested value cannot be encoded.  */
	      (*info->callbacks->reloc_overflow)
		(info, (h ? &h->root : NULL), name, howto->name,
		 (bfd_vma) 0, input_bfd, input_section, rel->r_offset);
	      break;

	    case bfd_reloc_undefined:
	      (*info->callbacks->undefined_symbol)
		(info, name, input_bfd, input_section,
		 rel->r_offset, true);
	      break;

	    case bfd_reloc_notsupported:
	      msg = _("internal error: unsupported relocation error");
	      goto common_error;

	    case bfd_reloc_dangerous:
	      msg = _("internal error: dangerous relocation");
	      goto common_error;

	    case bfd_reloc_other:
	      msg = _("could not locate special linker symbol __gp");
	      goto common_error;

	    case bfd_reloc_continue:
	      msg = _("could not locate special linker symbol __ep");
	      goto common_error;

/* add tazaki 2002.01.11 >>>>> */
		case bfd_reloc_over_doff_globalpointer:
	      msg = _("Default Data area pointer value is larger than symbol address value.");
	      goto common_error;

		case bfd_reloc_over_goff_globalpointer:
	      msg = _("G Data area pointer value is larger than symbol address value.");
	      goto common_error;

		case bfd_reloc_over_soff_globalpointer:
	      msg = _("S Data area pointer value is larger than symbol address value.");
	      goto common_error;

		case bfd_reloc_over_toff_globalpointer:
	      msg = _("T Data area pointer value is larger than symbol address value.");
	      goto common_error;

		case bfd_reloc_over_zoff_globalpointer:
	      msg = _("Z Data area pointer value is larger than symbol address value.");
	      goto common_error;

		case bfd_reloc_doff_over_64mb:
	      msg = _("The offset value of a symbol is over 64MB.(default data area)");
	      goto common_error;

		case bfd_reloc_doff_over_8kb:
	      msg = _("The offset value of a symbol is over 8KB.(default data area)");
	      goto common_error;

		case bfd_reloc_goff_over_8kb:
	      msg = _("The offset value of a symbol is over 8KB.(G data area)");
	      goto common_error;

		case bfd_reloc_soff_over_64mb:
	      msg = _("The offset value of a symbol is over 64MB.(S data area)");
	      goto common_error;

		case bfd_reloc_soff_over_8kb:
	      msg = _("The offset value of a symbol is over 8KB.(S data area)");
	      goto common_error;

		case bfd_reloc_toff_over_64mb:
	      msg = _("The offset value of a symbol is over 64MB.(T data area)");
	      goto common_error;

		case bfd_reloc_toff_over_8kb:
	      msg = _("The offset value of a symbol is over 8KB.(T data area)");
	      goto common_error;

		case bfd_reloc_zoff_over_64mb:
	      msg = _("The offset value of a symbol is over 64MB.(Z data area)");
	      goto common_error;

		case bfd_reloc_zoff_over_8kb:
	      msg = _("The offset value of a symbol is over 8KB.(Z data area)");
	      goto common_error;

		case bfd_reloc_dpoff_over_512kb:
	      msg = _("The offset value of a symbol is over 512KB.(default data area)");
	      goto common_error;
		
		case bfd_reloc_dpoff_over_64b:
	      msg = _("The offset value of a symbol is over 64byte.(default data area)");
	      goto common_error;
		
/* add tazaki 2002.01.11 <<<<< */

	    default:
	      msg = _("internal error: unknown error");
	      /* fall through */

	    common_error:
	      (*info->callbacks->warning)
		(info, msg, name, input_bfd, input_section,
		 rel->r_offset);
	      break;
	    }
	}
    }

  return true;
}



/* Store the machine number in the flags field.  */
/* Function to keep C33 specific file flags. */
static bool
c33_elf_set_private_flags (bfd * abfd, flagword flags)
{
  BFD_ASSERT (!elf_flags_init (abfd)
	      || elf_elfheader (abfd)->e_flags == flags);

  elf_elfheader (abfd)->e_flags = flags;
  elf_flags_init (abfd) = true;

  return true;
}

/* Copy backend specific data from one object module to another */
static bool
c33_elf_copy_private_bfd_data (bfd * ibfd, bfd * obfd)
{
  if (   bfd_get_flavour (ibfd) != bfd_target_elf_flavour
      || bfd_get_flavour (obfd) != bfd_target_elf_flavour)
    return true;

  BFD_ASSERT (!elf_flags_init (obfd)
	      || (elf_elfheader (obfd)->e_flags
		  == elf_elfheader (ibfd)->e_flags));

  elf_gp (obfd) = elf_gp (ibfd);
  elf_elfheader (obfd)->e_flags = elf_elfheader (ibfd)->e_flags;
  elf_flags_init (obfd) = true;

  /* Preserve generic ELF metadata as well, notably EI_OSABI and object
     attributes.  Bypassing this base hook made objcopy/strip turn GNU ELF
     inputs into System V objects and lose GNU section semantics.  */
  return _bfd_elf_copy_private_bfd_data (ibfd, obfd);
}


/* >>>>> ADDED D.Fujimoto 2007/10/01 */
static const char* c33_elf_get_mode_string(char mode_flag)
{
	switch (mode_flag) {
		case 'A': return "ADV";
		case 'P': return "PE";
		default : return "STD";
	}
}
/* <<<<< ADDED D.Fujimoto 2007/10/01 */

/* Merge backend specific data from an object file to the output
   object file when linking.  */
static bool
c33_elf_merge_private_bfd_data (bfd * ibfd, struct bfd_link_info * info)
{
  bfd * obfd = info->output_bfd;

  flagword out_flags;
  flagword in_flags;

/* >>>>> ADDED D.Fujimoto 2007/10/01 link error for mixing core object files */
	unsigned char mode_flag = 0;			// STD=0x0, PE='P', ADV='A'
	static unsigned char initial_mode_flag;
	static char initial_object_filename[512];
	static bool done_once = false;
/* <<<<< ADDED D.Fujimoto 2007/10/01 link error for mixing core object files */

  if (   bfd_get_flavour (ibfd) != bfd_target_elf_flavour
      || bfd_get_flavour (obfd) != bfd_target_elf_flavour)
    return true;

  in_flags = elf_elfheader (ibfd)->e_flags;
  out_flags = elf_elfheader (obfd)->e_flags;

/* >>>>> ADDED D.Fujimoto 2007/10/15 link error when input object files are not for C33 */
	if (elf_elfheader(ibfd)->e_machine != 0) {	// 0 will not be checked because of compatibility
		if (elf_elfheader(ibfd)->e_machine != EM_SE_C33) {
			fprintf (stderr, "Error: Input object file %s ", bfd_get_filename(ibfd));
			if (ibfd->my_archive != NULL) {
				fprintf(stderr, "included from %s ", bfd_get_filename (ibfd->my_archive));
			}
			fprintf(stderr, "is not for C33.\n");

			xexit(1);
		}
	}
/* <<<<< ADDED D.Fujimoto 2007/10/15 link error when input object files are not for C33 */

/* >>>>> ADDED D.Fujimoto 2007/10/01 link error for mixing core object files */
	// get the flag from the object file
	mode_flag = (unsigned char) (elf_elfheader(ibfd)->e_flags >> 24);

	// get initial mode
	if (!done_once) {
		initial_mode_flag = mode_flag;
		strncpy(initial_object_filename, bfd_get_filename(ibfd), 512);
		done_once = true;
	}

	// check mode
	if (mode_flag != initial_mode_flag) {

		// show an error and exit without creating executable
		fprintf(stderr, "Error: Cannot link %s object %s ", c33_elf_get_mode_string(mode_flag), bfd_get_filename(ibfd));
		if (ibfd->my_archive != NULL) {
			fprintf(stderr, "included from %s ", bfd_get_filename (ibfd->my_archive));
		}
		fprintf(stderr, "with %s object %s\n", c33_elf_get_mode_string(initial_mode_flag), initial_object_filename);
		xexit(1);

	}

	if (IS_FDPIC (obfd)
	    && (ibfd->flags & DYNAMIC) == 0
	    && (in_flags & EF_C33_FDPIC) == 0)
	  {
	    _bfd_error_handler
	      (_("%pB: not built for FDPIC (-mfdpic); it cannot be linked"
		 " into %pB"), ibfd, obfd);
	    bfd_set_error (bfd_error_wrong_format);
	    return false;
	  }

	// set ELF e_flags bit31-28 (e_machine is set in elf.c)
	elf_elfheader(obfd)->e_flags = initial_mode_flag << 24;
/* <<<<< ADDED D.Fujimoto 2007/10/01 link error for mixing core object files */

  if (! elf_flags_init (obfd))
    {
      /* If the input is the default architecture then do not
	 bother setting the flags for the output architecture,
	 instead allow future merges to do this.  If no future
	 merges ever set these flags then they will retain their
	 unitialised values, which surprise surprise, correspond
	 to the default values.  */
      if (bfd_get_arch_info (ibfd)->the_default)
	return true;
      
      elf_flags_init (obfd) = true;
      elf_elfheader (obfd)->e_flags = in_flags;

      if (bfd_get_arch (obfd) == bfd_get_arch (ibfd)
	  && bfd_get_arch_info (obfd)->the_default)
	{
	  return bfd_set_arch_mach (obfd, bfd_get_arch (ibfd), bfd_get_mach (ibfd));
	}

      return true;
    }

  /* Check flag compatibility.  */
  if (in_flags == out_flags)
    return true;
  return true;
}
/* Display the flags field */

static bool
c33_elf_print_private_bfd_data (bfd * abfd, void * ptr)
{

  
  BFD_ASSERT (abfd != NULL && ptr != NULL);
  
  _bfd_elf_print_private_bfd_data (abfd, ptr);
  return true;
}

/* C33 ELF uses four common sections.  One is the usual one, and the
   others are for (small) objects in one of the special data areas:
   small, tiny and zero.  All the objects are kept together, and then
   referenced via the gp register, the ep register or the r0 register
   respectively, which yields smaller, faster assembler code.  This
   approach is copied from elf32-mips.c.  */
/* del tazaki
static asection  c33_elf_scomm_section;
static asymbol   c33_elf_scomm_symbol;
static asymbol * c33_elf_scomm_symbol_ptr;
static asection  c33_elf_comm_section;
static asymbol   c33_elf_comm_symbol;
static asymbol * c33_elf_comm_symbol_ptr;
static asection  c33_elf_lcomm_section;
static asymbol   c33_elf_lcomm_symbol;
static asymbol * c33_elf_lcomm_symbol_ptr;
*/
static asection c33_elf_comm_section;
static const asymbol c33_elf_comm_symbol =
  GLOBAL_SYM_INIT (".comm", &c33_elf_comm_section);
static asection c33_elf_comm_section =
  BFD_FAKE_SECTION (c33_elf_comm_section, &c33_elf_comm_symbol,
		    ".comm", 0,
		    SEC_IS_COMMON | SEC_ALLOC | SEC_DATA);

static asection c33_elf_gcomm_section;
static const asymbol c33_elf_gcomm_symbol =
  GLOBAL_SYM_INIT (".gcomm", &c33_elf_gcomm_section);
static asection c33_elf_gcomm_section =
  BFD_FAKE_SECTION (c33_elf_gcomm_section, &c33_elf_gcomm_symbol,
		    ".gcomm", 0,
		    SEC_IS_COMMON | SEC_ALLOC | SEC_DATA);

static asection c33_elf_scomm_section;
static const asymbol c33_elf_scomm_symbol =
  GLOBAL_SYM_INIT (".scomm", &c33_elf_scomm_section);
static asection c33_elf_scomm_section =
  BFD_FAKE_SECTION (c33_elf_scomm_section, &c33_elf_scomm_symbol,
		    ".scomm", 0,
		    SEC_IS_COMMON | SEC_ALLOC | SEC_DATA);

static asection c33_elf_tcomm_section;
static const asymbol c33_elf_tcomm_symbol =
  GLOBAL_SYM_INIT (".tcomm", &c33_elf_tcomm_section);
static asection c33_elf_tcomm_section =
  BFD_FAKE_SECTION (c33_elf_tcomm_section, &c33_elf_tcomm_symbol,
		    ".tcomm", 0,
		    SEC_IS_COMMON | SEC_ALLOC | SEC_DATA);

static asection c33_elf_zcomm_section;
static const asymbol c33_elf_zcomm_symbol =
  GLOBAL_SYM_INIT (".zcomm", &c33_elf_zcomm_section);
static asection c33_elf_zcomm_section =
  BFD_FAKE_SECTION (c33_elf_zcomm_section, &c33_elf_zcomm_symbol,
		    ".zcomm", 0,
		    SEC_IS_COMMON | SEC_ALLOC | SEC_DATA);


/* del 2002/07/17 T.Tazaki >>> */
#if 0
static asection  c33_elf_gbss_section;
static asymbol   c33_elf_gbss_symbol;
static asymbol * c33_elf_gbss_symbol_ptr;
static asection  c33_elf_sbss_section;
static asymbol   c33_elf_sbss_symbol;
static asymbol * c33_elf_sbss_symbol_ptr;
static asection  c33_elf_tbss_section;
static asymbol   c33_elf_tbss_symbol;
static asymbol * c33_elf_tbss_symbol_ptr;
static asection  c33_elf_zbss_section;
static asymbol   c33_elf_zbss_symbol;
static asymbol * c33_elf_zbss_symbol_ptr;
#endif

/* Given a BFD section, try to locate the corresponding ELF section
   index.  */

static bool
c33_elf_section_from_bfd_section (bfd * abfd ATTRIBUTE_UNUSED,
                                  asection * sec,
                                  int * retval)
{
  if (strcmp (bfd_section_name (sec), ".comm") == 0)
    *retval = SHN_C33_COMM;
  else if (strcmp (bfd_section_name (sec), ".gcomm") == 0)
    *retval = SHN_C33_GCOMM;
  else if (strcmp (bfd_section_name (sec), ".scomm") == 0)
    *retval = SHN_C33_SCOMM;
  else if (strcmp (bfd_section_name (sec), ".tcomm") == 0)
    *retval = SHN_C33_TCOMM;
  else if (strcmp (bfd_section_name (sec), ".zcomm") == 0)
    *retval = SHN_C33_ZCOMM;
/* del 2002/07/17 T.Tazaki >>> */
#if 0
  else if (strcmp (bfd_section_name (sec), ".gbss") == 0)
    *retval = SHN_C33_GBSS;
  else if (strcmp (bfd_section_name (sec), ".sbss") == 0)
    *retval = SHN_C33_SBSS;
  else if (strcmp (bfd_section_name (sec), ".tbss") == 0)
    *retval = SHN_C33_TBSS;
  else if (strcmp (bfd_section_name (sec), ".zbss") == 0)
    *retval = SHN_C33_ZBSS;
#endif
  else
    return false;
  
  
  return true;
}

/* Handle the special c33 section numbers that a symbol may use.  */

static void
c33_elf_symbol_processing (bfd * abfd, asymbol * asym)
{
  elf_symbol_type * elfsym = (elf_symbol_type *) asym;
  unsigned int index;
  
  index = elfsym->internal_elf_sym.st_shndx;

  /* If the section index is an "ordinary" index, then it may
     refer to a c33 specific section created by the assembler.
     Check the section's type and change the index it matches.
     
     FIXME: Should we alter the st_shndx field as well ?  */
    /* Modify tazaki 2001.07.25 */
  
  if (index < elf_elfheader(abfd)[0].e_shnum)
    switch (elf_elfsections(abfd)[index]->sh_type)
      {
      case SHT_C33_COMM:
			index = SHN_C33_COMM;
			break;
      case SHT_C33_GCOMM:
			index = SHN_C33_GCOMM;
			break;
      case SHT_C33_SCOMM:
			index = SHN_C33_SCOMM;
			break;
      case SHT_C33_TCOMM:
			index = SHN_C33_TCOMM;
			break;
      case SHT_C33_ZCOMM:
			index = SHN_C33_ZCOMM;
			break;
/* del 2002/07/17 T.Tazaki >>> */
#if 0
      case SHT_C33_GBSS:
			index = SHN_C33_GBSS;
			break;
      case SHT_C33_SBSS:
			index = SHN_C33_SBSS;
			break;
      case SHT_C33_TBSS:
			index = SHN_C33_TBSS;
			break;
      case SHT_C33_ZBSS:
			index = SHN_C33_ZBSS;
			break;
#endif
      default:
			break;
      }
  
  switch (index)
    {
    case SHN_C33_COMM:
      asym->section = & c33_elf_comm_section;
      asym->value = elfsym->internal_elf_sym.st_size;
      break;

    case SHN_C33_GCOMM:
      asym->section = & c33_elf_gcomm_section;
      asym->value = elfsym->internal_elf_sym.st_size;
      break;

    case SHN_C33_SCOMM:
      asym->section = & c33_elf_scomm_section;
      asym->value = elfsym->internal_elf_sym.st_size;
      break;

    case SHN_C33_TCOMM:
      asym->section = & c33_elf_tcomm_section;
      asym->value = elfsym->internal_elf_sym.st_size;
      break;

    case SHN_C33_ZCOMM:
      asym->section = & c33_elf_zcomm_section;
      asym->value = elfsym->internal_elf_sym.st_size;
      break;
/* del 2002/07/17 T.Tazaki >>> */
#if 0
    case SHN_C33_GBSS:
      asym->section = & c33_elf_gbss_section;
      asym->value = elfsym->internal_elf_sym.st_size;
      break;

    case SHN_C33_SBSS:
      asym->section = & c33_elf_sbss_section;
      asym->value = elfsym->internal_elf_sym.st_size;
      break;

    case SHN_C33_TBSS:
      asym->section = & c33_elf_tbss_section;
      asym->value = elfsym->internal_elf_sym.st_size;
      break;

    case SHN_C33_ZBSS:
      asym->section = & c33_elf_zbss_section;
      asym->value = elfsym->internal_elf_sym.st_size;
      break;
#endif

/* gp�Q�Ɨp�ɕK�v�H */
/*
    case SHN_C33_SCOMMON:
      asym->section = & c33_elf_scomm_section;
      asym->value = elfsym->internal_elf_sym.st_size;
      break;
*/
    }
}

/* Hook called by the linker routine which adds symbols from an object
   file.  We must handle the special c33 section numbers here.  */

/*ARGSUSED*/
/* Modify tazaki 2001.07.25 */
static bool
c33_elf_add_symbol_hook (bfd * abfd,
                         struct bfd_link_info * info ATTRIBUTE_UNUSED,
                         Elf_Internal_Sym * sym,
                         const char ** namep ATTRIBUTE_UNUSED,
                         flagword * flagsp ATTRIBUTE_UNUSED,
                         asection ** secp,
                         bfd_vma * valp)
{
  unsigned int index = sym->st_shndx;
  
  /* If the section index is an "ordinary" index, then it may
     refer to a c33 specific section created by the assembler.
     Check the section's type and change the index it matches.
     
     FIXME: Should we alter the st_shndx field as well ?  */
  
  if (index < elf_elfheader(abfd)[0].e_shnum)
    switch (elf_elfsections(abfd)[index]->sh_type)
      {
    /* del tazaki 2001.07.25
      case SHT_C33_SCOMMON:
			index = SHN_C33_SCOMMON;
			break;
	*/
/* add tazaki 2001.11.19 >>>>> */
      case SHT_C33_COMM:
			index = SHN_C33_COMM;
			break;
      case SHT_C33_GCOMM:
			index = SHN_C33_GCOMM;
			break;
      case SHT_C33_SCOMM:
			index = SHN_C33_SCOMM;
			break;
      case SHT_C33_TCOMM:
			index = SHN_C33_TCOMM;
			break;
      case SHT_C33_ZCOMM:
			index = SHN_C33_ZCOMM;
			break;
/* del 2002/07/17 T.Tazaki >>> */
#if 0
      case SHT_C33_GBSS:
			index = SHN_C33_GBSS;
			break;
      case SHT_C33_SBSS:
			index = SHN_C33_SBSS;
			break;
      case SHT_C33_TBSS:
			index = SHN_C33_TBSS;
			break;
      case SHT_C33_ZBSS:
			index = SHN_C33_ZBSS;
			break;
#endif
/* add tazaki 2001.11.19 <<<<< */
	
      default:
	break;
      }
  
  switch (index)
    {
/* del tazaki 2001.07.25
    case SHN_C33_SCOMMON:
      *secp = bfd_make_section_old_way (abfd, ".scommon");
      (*secp)->flags |= SEC_IS_COMMON;
      *valp = sym->st_size;
      break;
*/    
/* add tazaki 2001.11.19 >>>>> */
    case SHN_C33_COMM:
      *secp = bfd_make_section_old_way (abfd, ".comm");
      (*secp)->flags |= SEC_IS_COMMON;
      *valp = sym->st_size;
      break;
      
    case SHN_C33_GCOMM:
      *secp = bfd_make_section_old_way (abfd, ".gcomm");
      (*secp)->flags |= SEC_IS_COMMON;
      *valp = sym->st_size;
      break;

    case SHN_C33_SCOMM:
      *secp = bfd_make_section_old_way (abfd, ".scomm");
      (*secp)->flags |= SEC_IS_COMMON;
      *valp = sym->st_size;
      break;

    case SHN_C33_TCOMM:
      *secp = bfd_make_section_old_way (abfd, ".tcomm");
      (*secp)->flags |= SEC_IS_COMMON;
      *valp = sym->st_size;
      break;

    case SHN_C33_ZCOMM:
      *secp = bfd_make_section_old_way (abfd, ".zcomm");
      (*secp)->flags |= SEC_IS_COMMON;
      *valp = sym->st_size;
      break;
/* del 2002/07/17 T.Tazaki >>> */
#if 0
    case SHN_C33_GBSS:
      *secp = bfd_make_section_old_way (abfd, ".gbss");
      (*secp)->flags |= SEC_IS_COMMON;
      *valp = sym->st_size;
      break;

    case SHN_C33_SBSS:
      *secp = bfd_make_section_old_way (abfd, ".sbss");
      (*secp)->flags |= SEC_IS_COMMON;
      *valp = sym->st_size;
      break;

    case SHN_C33_TBSS:
      *secp = bfd_make_section_old_way (abfd, ".tbss");
      (*secp)->flags |= SEC_IS_COMMON;
      *valp = sym->st_size;
      break;

    case SHN_C33_ZBSS:
      *secp = bfd_make_section_old_way (abfd, ".zbss");
      (*secp)->flags |= SEC_IS_COMMON;
      *valp = sym->st_size;
      break;
#endif

/* add tazaki 2001.11.19 <<<<< */
    }

  return true;
}

/*ARGSIGNORED*/
static int
c33_elf_link_output_symbol_hook (struct bfd_link_info * info ATTRIBUTE_UNUSED,
                                 const char * name ATTRIBUTE_UNUSED,
                                 Elf_Internal_Sym * sym,
                                 asection * input_sec,
                                 struct elf_link_hash_entry * h ATTRIBUTE_UNUSED)
{
  /* If we see a common symbol, which implies a relocatable link, then
     if a symbol was in a special common section in an input file, mark
     it as a special common in the output file.  */
  
  if (sym->st_shndx == SHN_COMMON)
    {
      if (strcmp (input_sec->name, ".comm") == 0)
	sym->st_shndx = SHN_C33_COMM;
      else if (strcmp (input_sec->name, ".gcomm") == 0)
	sym->st_shndx = SHN_C33_GCOMM;
      else if (strcmp (input_sec->name, ".scomm") == 0)
	sym->st_shndx = SHN_C33_SCOMM;
      else if (strcmp (input_sec->name, ".tcomm") == 0)
	sym->st_shndx = SHN_C33_TCOMM;
      else if (strcmp (input_sec->name, ".zcomm") == 0)
	sym->st_shndx = SHN_C33_ZCOMM;
/* del 2002/07/17 T.Tazaki >>> */
#if 0
      else if (strcmp (input_sec->name, ".gbss") == 0)
	sym->st_shndx = SHN_C33_GBSS;
      else if (strcmp (input_sec->name, ".sbss") == 0)
	sym->st_shndx = SHN_C33_SBSS;
      else if (strcmp (input_sec->name, ".tbss") == 0)
	sym->st_shndx = SHN_C33_TBSS;
      else if (strcmp (input_sec->name, ".zbss") == 0)
	sym->st_shndx = SHN_C33_ZBSS;
#endif
    }

  return true;
}

/* Modify tazaki 2001.07.25 */
static bool
c33_elf_section_from_shdr (bfd * abfd, Elf_Internal_Shdr * hdr,
                           const char * name, int shindex)
{
  /* There ought to be a place to keep ELF backend specific flags, but
     at the moment there isn't one.  We just keep track of the
     sections by their name, instead.  */

  if (! _bfd_elf_make_section_from_shdr (abfd, hdr, name, shindex))
    return false;

  switch (hdr->sh_type)
    {
/*    case SHT_C33_SCOMMON:	*/
    case SHT_C33_COMM:
    case SHT_C33_GCOMM:
    case SHT_C33_SCOMM:
    case SHT_C33_TCOMM:
    case SHT_C33_ZCOMM:
/* del 2002/07/17 T.Tazaki >>> */
#if 0
    case SHT_C33_GBSS:
    case SHT_C33_SBSS:
    case SHT_C33_TBSS:
    case SHT_C33_ZBSS:
#endif
      if (! bfd_set_section_flags (hdr->bfd_section,
				   (bfd_section_flags (hdr->bfd_section)
				    | SEC_IS_COMMON)))
	return false;
    }

  return true;
}

/* Set the correct type for a C33 ELF section.  We do this by the
   section name, which is a hack, but ought to work.  */

/* Modify tazaki 2001.11.19 >>>>> */

static bool
c33_elf_fake_sections (bfd * abfd ATTRIBUTE_UNUSED,
                       Elf_Internal_Shdr * hdr,
                       asection * sec)
{
  register const char * name;

  name = bfd_section_name (sec);

  if (strcmp (name, ".comm") == 0)
    {
      hdr->sh_type = SHT_C33_COMM;
    }
  else if (strcmp (name, ".gcomm") == 0)
    {
      hdr->sh_type = SHT_C33_GCOMM;
    }
  else if (strcmp (name, ".scomm") == 0)
    {
      hdr->sh_type = SHT_C33_SCOMM;
    }
  else if (strcmp (name, ".tcomm") == 0)
    {
      hdr->sh_type = SHT_C33_TCOMM;
    }
  else if (strcmp (name, ".zcomm") == 0)
    {
      hdr->sh_type = SHT_C33_ZCOMM;
    }
/* del 2002/07/17 T.Tazaki >>> */
#if 0
  else if (strcmp (name, ".gbss") == 0)
    {
      hdr->sh_type = SHT_C33_GBSS;
    }
  else if (strcmp (name, ".sbss") == 0)
    {
      hdr->sh_type = SHT_C33_SBSS;
    }
  else if (strcmp (name, ".tbss") == 0)
    {
      hdr->sh_type = SHT_C33_TBSS;
    }
  else if (strcmp (name, ".zbss") == 0)
    {
      hdr->sh_type = SHT_C33_ZBSS;
    }
#endif 
  
  return true;
}
/* Modify tazaki 2001.11.19 <<<<< */



#define TARGET_LITTLE_SYM			c33_elf32_vec
#define TARGET_LITTLE_NAME			"elf32-c33"
#define ELF_ARCH					bfd_arch_c33
/* >>>>> MODIFIED D.Fujimoto 2007/10/15 machine code */
/* EM_SE_C33 is 107, allocated to Seiko Epson upstream (include/elf/common.h).
   The original toolchain wrote it too -- every shipped object and
   ROOT_IMAGE/kernel.elf carries it -- but it got there by patching a switch
   in the shared bfd/elf.c, so this file was left saying EM_NONE with a "do
   not change" note.  That switch is gone in modern bfd, which writes
   bed->elf_machine_code directly, so the value has to be right here.

   Note the side effect: with EM_NONE, elfcode.h accepts an object of any
   machine (see the EM_NONE special case in elf_object_p).  Now we only
   accept 107, which is what every real C33 object has.  */
#define ELF_MACHINE_CODE			EM_SE_C33
/* <<<<< MODIFIED D.Fujimoto 2007/10/15 machine code */
#define ELF_MAXPAGESIZE				0x1000
	
#define elf_info_to_howto			c33_elf_info_to_howto_rela
#define elf_info_to_howto_rel			c33_elf_info_to_howto_rel

#define elf_backend_check_relocs		c33_elf_check_relocs
#define elf_backend_relocate_section    	c33_elf_relocate_section

#define elf_backend_object_p			c33_elf_object_p

#define elf_backend_section_from_bfd_section 	c33_elf_section_from_bfd_section
#define elf_backend_symbol_processing		c33_elf_symbol_processing
#define elf_backend_add_symbol_hook		c33_elf_add_symbol_hook
#define elf_backend_link_output_symbol_hook 	c33_elf_link_output_symbol_hook
#define elf_backend_section_from_shdr		c33_elf_section_from_shdr
#define elf_backend_fake_sections		c33_elf_fake_sections
#define elf_backend_sym_is_global		c33_elf_sym_is_global

#define elf_backend_can_gc_sections 1


#define bfd_elf32_bfd_is_local_label_name	c33_elf_is_local_label_name
#define bfd_elf32_bfd_reloc_type_lookup		c33_elf_reloc_type_lookup
#define bfd_elf32_bfd_reloc_name_lookup		c33_elf_reloc_name_lookup
#define bfd_elf32_bfd_copy_private_bfd_data 	c33_elf_copy_private_bfd_data
#define bfd_elf32_bfd_merge_private_bfd_data 	c33_elf_merge_private_bfd_data
#define bfd_elf32_bfd_set_private_flags		c33_elf_set_private_flags
#define bfd_elf32_bfd_print_private_bfd_data	c33_elf_print_private_bfd_data

/* C33 ELF uses the symbol spelling emitted by GCC verbatim.  The legacy
   backend used '#' here as a map-file workaround, but modern ld treats any
   nonzero value as a real ABI prefix.  That prevented generation of normal
   __start_/__stop_ symbols (and disagreed with USER_LABEL_PREFIX in GCC).  */
#define elf_symbol_leading_char			0

#include "elf32-target.h"

/* Text and data load independently, so a pointer from .eh_frame to code
   cannot become PC-relative. Keep the absolute relocations emitted by GCC,
   including personality and LSDA pointers. */
static bool
c33fdpic_can_make_relative_eh_frame (bfd *abfd ATTRIBUTE_UNUSED,
                                   struct bfd_link_info *info ATTRIBUTE_UNUSED,
                                   asection *sec ATTRIBUTE_UNUSED)
{
  return false;
}

/* FDPIC: the same relocations, plus executables and shared libraries for
   ld.so and the kernel's ELF FDPIC loader.  */

#undef TARGET_LITTLE_SYM
#define TARGET_LITTLE_SYM			c33_elf32_fdpic_vec
#undef TARGET_LITTLE_NAME
#define TARGET_LITTLE_NAME			"elf32-c33fdpic"
#undef elf32_bed
#define elf32_bed				elf32_c33fdpic_bed

#undef elf_backend_check_relocs
#define elf_backend_check_relocs		c33fdpic_check_relocs
#undef bfd_elf32_bfd_link_hash_table_create
#define bfd_elf32_bfd_link_hash_table_create	c33fdpic_link_hash_table_create
#undef elf_backend_copy_indirect_symbol
#define elf_backend_copy_indirect_symbol	c33fdpic_copy_indirect_symbol
#undef elf_backend_create_dynamic_sections
#define elf_backend_create_dynamic_sections	c33fdpic_create_dynamic_sections
#undef elf_backend_adjust_dynamic_symbol
#define elf_backend_adjust_dynamic_symbol	c33fdpic_adjust_dynamic_symbol
#undef elf_backend_late_size_sections
#define elf_backend_late_size_sections		c33fdpic_late_size_sections
#undef elf_backend_finish_dynamic_symbol
#define elf_backend_finish_dynamic_symbol	c33fdpic_finish_dynamic_symbol
#undef elf_backend_finish_dynamic_sections
#define elf_backend_finish_dynamic_sections	c33fdpic_finish_dynamic_sections
#undef elf_backend_discard_info
#define elf_backend_discard_info		c33fdpic_discard_info
#undef elf_backend_final_write_processing
#define elf_backend_final_write_processing	c33fdpic_final_write_processing
#undef elf_backend_may_use_rel_p
#define elf_backend_may_use_rel_p		0
#undef elf_backend_may_use_rela_p
#define elf_backend_may_use_rela_p		1
#undef elf_backend_default_use_rela_p
#define elf_backend_default_use_rela_p		1
#undef elf_backend_want_got_plt
#define elf_backend_want_got_plt		0
#undef elf_backend_plt_readonly
#define elf_backend_plt_readonly		1
#undef elf_backend_want_plt_sym
#define elf_backend_want_plt_sym		0
#undef elf_backend_got_header_size
#define elf_backend_got_header_size		0

#undef elf_backend_can_make_relative_eh_frame
#define elf_backend_can_make_relative_eh_frame \
  c33fdpic_can_make_relative_eh_frame

#include "elf32-target.h"
