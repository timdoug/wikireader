#as: -mc33pe
#readelf: -wf
#name: C33 call frame information

Contents of the .eh_frame section:


0+0 0+14 0+0 CIE
  Version:               1
  Augmentation:          "zR"
  Code alignment factor: 2
  Data alignment factor: -4
  Return address column: 22
  Augmentation data:     1b
  DW_CFA_def_cfa: r16 ofs 4
  DW_CFA_offset: r22 at cfa-4
  DW_CFA_nop
  DW_CFA_nop

0+18 0+24 0+1c FDE cie=0+0 pc=0+0..0+210
  DW_CFA_advance_loc: 2 to 0+2
  DW_CFA_def_cfa_offset: 12
  DW_CFA_offset: r1 at cfa-8
  DW_CFA_offset: r0 at cfa-12
  DW_CFA_advance_loc: 2 to 0+4
  DW_CFA_def_cfa_offset: 20
  DW_CFA_advance_loc2: 520 to 0+20c
  DW_CFA_def_cfa_offset: 12
  DW_CFA_advance_loc: 2 to 0+20e
  DW_CFA_def_cfa_offset: 4
  DW_CFA_restore: r0
  DW_CFA_restore: r1
  DW_CFA_nop
  DW_CFA_nop
  DW_CFA_nop

0+40 0+20 0+44 FDE cie=0+0 pc=0+210..0+218
  DW_CFA_advance_loc: 2 to 0+212
  DW_CFA_def_cfa_offset: 0
  DW_CFA_register: r22 in r4
  DW_CFA_advance_loc: 2 to 0+214
  DW_CFA_def_cfa_offset: 4
  DW_CFA_offset: r22 at cfa-4
  DW_CFA_advance_loc: 2 to 0+216
  DW_CFA_def_cfa_register: r12
  DW_CFA_offset: r17 at cfa-4
  DW_CFA_offset: r18 at cfa\+0
  DW_CFA_nop
