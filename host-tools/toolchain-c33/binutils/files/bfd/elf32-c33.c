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
	 false)				/* pcrel_offset */

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
  { BFD_RELOC_C33_PUSH_R1,	R_C33_PUSH_R1 }   /* add tazaki 2004/08/19 */
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
  return ((name[0] == '_' && name[1] == '_' && name[2] == 'L'));
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

  if (sym_hashes == NULL)
    {
      info->callbacks->warning
	(info, "no hash table available", NULL, input_bfd, input_section, 0);

      return false;
    }
  
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

      r_symndx = ELF32_R_SYM (rel->r_info);
      r_type   = ELF32_R_TYPE (rel->r_info);
      howto = c33_elf_howto_table + r_type;

      if (bfd_link_relocatable (info))
	{
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
	  relocation = (sec->output_section->vma
			+ sec->output_offset
			+ sym->st_value);
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
	  h = sym_hashes[r_symndx - symtab_hdr->sh_info];
	  
	  while (h->root.type == bfd_link_hash_indirect
		 || h->root.type == bfd_link_hash_warning)
	    h = (struct elf_link_hash_entry *) h->root.u.i.link;
	  
	  if (h->root.type == bfd_link_hash_defined
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
	    }
	  else
	    {
	      (*info->callbacks->undefined_symbol)
		(info, h->root.root.string, input_bfd,
		 input_section, rel->r_offset, true);
#if 0
fprintf (stderr, "unknown: name: %s\n", h->root.root.string);
#endif
	      relocation = 0;
	    }
	}

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
	      (*info->callbacks->reloc_overflow)
		(info, (h ? &h->root : NULL), name, howto->name,
		 (bfd_vma) 0, input_bfd, input_section, rel->r_offset);
	      break;

	    case bfd_reloc_undefined:
	      (*info->callbacks->undefined_symbol)
		(info, name, input_bfd, input_section,
		 rel->r_offset, true);
	      break;

	    case bfd_reloc_outofrange:
//	      msg = _("internal error: out of range error");		/* Change T.Tazaki 2003/11/18 */
	      msg = _("out of range error");						/* Change T.Tazaki 2003/11/18 */
	      goto common_error;

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
  return true;
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
  unsigned short index;
  
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
  int index = sym->st_shndx;
  
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

#if 0
#define elf_backend_object_p			c33_elf_object_p
#define elf_backend_final_write_processing 	c33_elf_final_write_processing
#endif

#define elf_backend_section_from_bfd_section 	c33_elf_section_from_bfd_section
#define elf_backend_symbol_processing		c33_elf_symbol_processing
#define elf_backend_add_symbol_hook		c33_elf_add_symbol_hook
#define elf_backend_link_output_symbol_hook 	c33_elf_link_output_symbol_hook
#define elf_backend_section_from_shdr		c33_elf_section_from_shdr
#define elf_backend_fake_sections		c33_elf_fake_sections

#define elf_backend_can_gc_sections 1


#define bfd_elf32_bfd_is_local_label_name	c33_elf_is_local_label_name
#define bfd_elf32_bfd_reloc_type_lookup		c33_elf_reloc_type_lookup
#define bfd_elf32_bfd_reloc_name_lookup		c33_elf_reloc_name_lookup
#define bfd_elf32_bfd_copy_private_bfd_data 	c33_elf_copy_private_bfd_data
#define bfd_elf32_bfd_merge_private_bfd_data 	c33_elf_merge_private_bfd_data
#define bfd_elf32_bfd_set_private_flags		c33_elf_set_private_flags
#define bfd_elf32_bfd_print_private_bfd_data	c33_elf_print_private_bfd_data

/* change leading_char for map file  T.Tazaki 2003/12/09 >>> */
//#define elf_symbol_leading_char			'_'
#define elf_symbol_leading_char			'#'
/* change T.Tazaki 2003/12/09 <<< */

#include "elf32-target.h"
