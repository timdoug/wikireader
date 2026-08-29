/* C33 ELF support for BFD.
   Copyright (C) 1997,2001 Free Software Foundation, Inc.
   Created by Michael Meissner, Cygnus Support <meissner@cygnus.com>

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

/* This file holds definitions specific to the MIPS ELF ABI.  Note
   that most of this is not actually implemented by BFD.  */

#ifndef _ELF_C33_H
#define _ELF_C33_H

/* Processor specific flags for the ELF header e_flags field.  */
#if 0	/* c33 */
/* ������binutils\readelf.c�Ŏg�p���Ă��� */
/*	get_machine_flags()��ELF�w�b�_��e_flags�ɂ���āA�A�[�L�e�N�`���̕������ */
/*	�ݒ肵�Ă���B���R�R�̏ꍇ�͂ǂ������炢�����H	*/

/* Four bit C33 architecture field.  */
#define EF_V850_ARCH		0xf0000000

/* v850 code.  */
#define E_V850_ARCH		0x00000000

/* v850e code.  */
#define E_V850E_ARCH		0x10000000

/* v850ea code.  */
#define E_V850EA_ARCH		0x20000000
#endif	/* c33 */


/* Flags for the st_other field */
#define C33_OTHER_SDA		0x01	/* symbol had SDA relocations */
#define C33_OTHER_ZDA		0x02	/* symbol had ZDA relocations */
#define C33_OTHER_TDA		0x04	/* symbol had TDA relocations */
#define C33_OTHER_TDA_BYTE	0x08	/* symbol had TDA byte relocations */
#define C33_OTHER_ERROR		0x80	/* symbol had an error reported */

/* C33 relocations */
#include "elf/reloc-macros.h"

START_RELOC_NUMBERS (c33_reloc_type)
     RELOC_NUMBER (R_C33_NONE, 0)
     RELOC_NUMBER (R_C33_32, 1)
     RELOC_NUMBER (R_C33_16, 2)
     RELOC_NUMBER (R_C33_8,  3)
     RELOC_NUMBER (R_C33_AH, 4)
     RELOC_NUMBER (R_C33_AL, 5)
     RELOC_NUMBER (R_C33_RH, 6)
     RELOC_NUMBER (R_C33_RM, 7)
     RELOC_NUMBER (R_C33_RL, 8)
     RELOC_NUMBER (R_C33_H,  9)
     RELOC_NUMBER (R_C33_M,  10)
     RELOC_NUMBER (R_C33_L,  11)
     RELOC_NUMBER (R_C33_DH, 12)	/* add tazaki 2002.01.11 */
     RELOC_NUMBER (R_C33_DL, 13)	/* add tazaki 2002.01.11 */
     RELOC_NUMBER (R_C33_GL, 14)	/* add tazaki 2002.01.11 */
     RELOC_NUMBER (R_C33_SH, 15)	/* add tazaki 2002.01.11 */
     RELOC_NUMBER (R_C33_SL, 16)	/* add tazaki 2002.01.11 */
     RELOC_NUMBER (R_C33_TH, 17)	/* add tazaki 2002.01.11 */
     RELOC_NUMBER (R_C33_TL, 18)	/* add tazaki 2002.01.11 */
     RELOC_NUMBER (R_C33_ZH, 19)	/* add tazaki 2002.01.11 */
     RELOC_NUMBER (R_C33_ZL, 20)	/* add tazaki 2002.01.11 */
     RELOC_NUMBER (R_C33_DPH,21)	/* add tazaki 2002.01.11 */
     RELOC_NUMBER (R_C33_DPM,22)	/* add tazaki 2002.01.11 */
     RELOC_NUMBER (R_C33_DPL,23)	/* add tazaki 2002.01.11 */
     RELOC_NUMBER (R_C33_LOOP,24)	/* add tazaki 2002.03.05 */
     RELOC_NUMBER (R_C33_JP, 25)	/* add tazaki 2002.04.22 */
     RELOC_NUMBER (R_C33_S_RH, 26)	/* add tazaki 2002.05.02 */
     RELOC_NUMBER (R_C33_S_RM, 27)	/* add tazaki 2002.05.02 */
     RELOC_NUMBER (R_C33_S_RL, 28)	/* add tazaki 2002.05.02 */
     RELOC_NUMBER (R_C33_PUSHN_R0,29)	/* add tazaki 2004/08/19 */
     RELOC_NUMBER (R_C33_PUSHN_R1,30)	/* add tazaki 2004/08/19 */
     RELOC_NUMBER (R_C33_PUSH_R1,31)	/* add tazaki 2004/08/19 */

END_RELOC_NUMBERS (R_C33_max)


/* Processor specific section indices.  These sections do not actually
   exist.  Symbols with a st_shndx field corresponding to one of these
   values have a special meaning.  */

/* Small data area common symbol.  */
#define SHN_C33_COMM	(SHN_LORESERVE + 0)
#define SHN_C33_GCOMM	(SHN_LORESERVE + 1)
#define SHN_C33_SCOMM	(SHN_LORESERVE + 2)
#define SHN_C33_TCOMM	(SHN_LORESERVE + 3)
#define SHN_C33_ZCOMM	(SHN_LORESERVE + 4)
#define SHN_C33_GBSS	(SHN_LORESERVE + 5)
#define SHN_C33_SBSS	(SHN_LORESERVE + 6)
#define SHN_C33_TBSS	(SHN_LORESERVE + 7)
#define SHN_C33_ZBSS	(SHN_LORESERVE + 8)


/* Processor specific section types.  */

/* Section contains the .scommon data.  */
#define SHT_C33_COMM	0x70000000
#define SHT_C33_GCOMM	0x70000001
#define SHT_C33_SCOMM	0x70000002
#define SHT_C33_TCOMM	0x70000003
#define SHT_C33_ZCOMM	0x70000004
#define SHT_C33_GBSS	0x70000005
#define SHT_C33_SBSS	0x70000006
#define SHT_C33_TBSS	0x70000007
#define SHT_C33_ZBSS	0x70000008



#endif /* _ELF_C33_H */
