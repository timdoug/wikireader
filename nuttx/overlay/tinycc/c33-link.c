/* Epson C33 ELF relocations. SPDX-License-Identifier: LGPL-2.0-or-later */
#ifdef TARGET_DEFS_ONLY
#define EM_TCC_TARGET 107
#define R_C33_32 1
#define R_C33_16 2
#define R_C33_8 3
#define R_C33_H 9
#define R_C33_M 10
#define R_C33_L 11
#define R_DATA_32 R_C33_32
#define R_DATA_PTR R_C33_32
/* Reserved values: dynamic ELF linking is deliberately unsupported. */
#define R_JMP_SLOT 32
#define R_GLOB_DAT 33
#define R_COPY 34
#define R_RELATIVE 35
#define R_NUM 36
#define PCRELATIVE_DLLPLT 0
#define RELOCATE_DLLPLT 0
#define ELF_START_ADDR 0x10000000
#define ELF_PAGE_SIZE 0x1000
#else
#include "tcc.h"
ST_FUNC int code_reloc(int t)
{
    if (t >= R_C33_H && t <= R_C33_L) return 1;
    if (t >= R_C33_32 && t <= R_C33_8) return 0;
    return -1;
}
ST_FUNC int gotplt_entry_type(int t) { return NO_GOTPLT_ENTRY; }
ST_FUNC unsigned create_plt_entry(TCCState *s1, unsigned off, struct sym_attr *attr)
{
    tcc_error_noabort("C33: dynamic ELF linking is not supported");
    return 0;
}
ST_FUNC void relocate_plt(TCCState *s) { }
ST_FUNC void relocate(TCCState *s1, ElfW_Rel *rel, int t, unsigned char *p,
                      addr_t addr, addr_t val)
{
    switch (t) {
    case 0: break;
    case R_C33_32: add32le(p, val); break;
    case R_C33_16: write16le(p, read16le(p) + val); break;
    case R_C33_8: *p += val; break;
    case R_C33_H: write16le(p, (read16le(p) & 0xe000) | ((val >> 19) & 8191)); break;
    case R_C33_M: write16le(p, (read16le(p) & 0xe000) | ((val >> 6) & 8191)); break;
    case R_C33_L: write16le(p, (read16le(p) & 0xfc0f) | ((val & 63) << 4)); break;
    default: tcc_error_noabort("C33: unsupported ELF relocation %d", t);
    }
}
#endif
