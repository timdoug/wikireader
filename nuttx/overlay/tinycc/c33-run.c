/* Flat-address-space NuttX runtime. SPDX-License-Identifier: LGPL-2.0-or-later */
#include "tcc.h"
#ifdef TCC_IS_NATIVE
ST_FUNC void *dlopen(const char *name, int flags) { return NULL; }
ST_FUNC void dlclose(void *handle) { }
ST_FUNC const char *dlerror(void) { return "NuttX: shared libraries unavailable"; }
ST_FUNC void *dlsym(void *handle, const char *name) { return NULL; }

LIBTCCAPI int tcc_relocate(TCCState *s1)
{
    unsigned i, size = 0;
    unsigned char *mem;
    if (s1->run_ptr)
        return tcc_error_noabort("already relocated");
    resolve_common_syms(s1);
    for (i = 1; i < s1->nb_sections; i++) {
        Section *sec = s1->sections[i];
        if (!(sec->sh_flags & SHF_ALLOC)) continue;
        if (sec->sh_type == SHT_INIT_ARRAY || sec->sh_type == SHT_FINI_ARRAY)
            return tcc_error_noabort("C33: constructors/destructors are unsupported");
        if (sec->sh_flags & SHF_TLS)
            return tcc_error_noabort("C33: TLS is unsupported");
        if (sec->sh_addralign > 16)
            return tcc_error_noabort("C33: alignment greater than 16 is unsupported");
        size = (size + 15) & -16;
        sec->sh_addr = size;
        size += sec->data_offset;
    }
    s1->run_ptr = mem = tcc_malloc(size + 15);
    s1->run_size = size + 15;
    mem = (void *)(((uintptr_t)mem + 15) & -(uintptr_t)16);
    for (i = 1; i < s1->nb_sections; i++) {
        Section *sec = s1->sections[i];
        if (sec->sh_flags & SHF_ALLOC) sec->sh_addr += (addr_t)mem;
    }
    relocate_syms(s1, s1->symtab, 1);
    if (s1->nb_errors) return -1;
    relocate_sections(s1);
    if (s1->nb_errors) return -1;
    for (i = 1; i < s1->nb_sections; i++) {
        Section *sec = s1->sections[i];
        if (!(sec->sh_flags & SHF_ALLOC)) continue;
        if (sec->sh_type == SHT_NOBITS || !sec->data)
            memset((void *)sec->sh_addr, 0, sec->data_offset);
        else memcpy((void *)sec->sh_addr, sec->data, sec->data_offset);
    }
    /* The current S1C33E07 NuttX port runs uncached SDRAM. */
    return 0;
}
ST_FUNC void tcc_run_free(TCCState *s1)
{
    tcc_free(s1->run_ptr);
    s1->run_ptr = NULL;
}
#endif
