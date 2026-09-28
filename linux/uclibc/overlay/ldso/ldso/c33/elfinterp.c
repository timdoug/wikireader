/* C33 FDPIC shared library loader support.
 *
 * Licensed under the LGPL v2.1, see the file COPYING.LIB in this tarball.
 *
 * Modelled on the FR-V loader.  Relocations are RELA and all resolved at
 * load time:
 *
 *   R_C33_32		a word: SYM + addend, or with no symbol, the module's
 *			own link-time address in the addend, relocated
 *			through its load map;
 *   R_C33_FUNCDESC	a word: the address of SYM's canonical descriptor;
 *   R_C33_FUNCDESC_VALUE
 *			a .got descriptor, {SYM, SYM's module's %r15}, or with
 *			no symbol, {the addend relocated, this module's}.
 */

#include <sys/cdefs.h>
#include <features.h>

static int
_dl_parse(struct elf_resolve *tpnt, struct r_scope_elem *scope,
	  unsigned long rel_addr, unsigned long rel_size,
	  int (*reloc_fnc) (struct elf_resolve *tpnt, struct r_scope_elem *scope,
			    ELF_RELOC *rpnt, ElfW(Sym) *symtab, char *strtab))
{
	unsigned int i;
	char *strtab;
	ElfW(Sym) *symtab;
	ELF_RELOC *rpnt;
	int symtab_index;

	rpnt = (ELF_RELOC *) rel_addr;
	rel_size = rel_size / sizeof(ELF_RELOC);

	symtab = (ElfW(Sym) *) tpnt->dynamic_info[DT_SYMTAB];
	strtab = (char *) tpnt->dynamic_info[DT_STRTAB];

	for (i = 0; i < rel_size; i++, rpnt++) {
		int res;

		symtab_index = ELF_R_SYM(rpnt->r_info);
		debug_sym(symtab, strtab, symtab_index);
		debug_reloc(symtab, strtab, rpnt);

		res = reloc_fnc(tpnt, scope, rpnt, symtab, strtab);
		if (res == 0)
			continue;

		_dl_dprintf(2, "\n%s: ", _dl_progname);
		if (symtab_index)
			_dl_dprintf(2, "symbol '%s': ",
				    strtab + symtab[symtab_index].st_name);
		if (res < 0) {
			int reloc_type = ELF_R_TYPE(rpnt->r_info);

			_dl_dprintf(2, "can't handle reloc type %x\n", reloc_type);
			_dl_exit(-res);
		} else if (res > 0) {
			_dl_dprintf(2, "can't resolve symbol\n");
			return res;
		}
	}
	return 0;
}

static int
_dl_do_reloc(struct elf_resolve *tpnt, struct r_scope_elem *scope,
	     ELF_RELOC *rpnt, ElfW(Sym) *symtab, char *strtab)
{
	int reloc_type;
	int symtab_index;
	char *symname;
	unsigned long *reloc_addr;
	unsigned long reloc_value = 0;
	unsigned long symbol_addr = 0;
	struct elf_resolve *symbol_tpnt = tpnt;
	struct funcdesc_value funcval;
	struct symbol_ref sym_ref;
#if defined (__SUPPORT_LD_DEBUG__)
	unsigned long old_val;
#endif

	reloc_addr = (unsigned long *) DL_RELOC_ADDR(tpnt->loadaddr, rpnt->r_offset);
	reloc_type = ELF_R_TYPE(rpnt->r_info);
	symtab_index = ELF_R_SYM(rpnt->r_info);
	sym_ref.sym = &symtab[symtab_index];
	sym_ref.tpnt = NULL;
	symname = strtab + symtab[symtab_index].st_name;

	if (symtab_index == 0) {
		/* The module's own address, in the addend.  */
		symbol_addr = (unsigned long) DL_RELOC_ADDR(tpnt->loadaddr, rpnt->r_addend);
	} else if (ELF_ST_BIND(symtab[symtab_index].st_info) == STB_LOCAL) {
		symbol_addr = (unsigned long) DL_RELOC_ADDR(tpnt->loadaddr,
			symtab[symtab_index].st_value) + rpnt->r_addend;
	} else {
		symbol_addr = (unsigned long)
			_dl_find_hash(symname, scope, NULL, 0, &sym_ref);
		/* Undefined weak references are allowed, and are zero.  */
		if (!symbol_addr
		    && ELF_ST_BIND(symtab[symtab_index].st_info) != STB_WEAK) {
			_dl_dprintf(2, "%s: can't resolve symbol '%s'\n",
				    _dl_progname, symname);
			_dl_exit(1);
		}
		symbol_tpnt = sym_ref.tpnt;
		if (symbol_addr)
			symbol_addr += rpnt->r_addend;
	}

#if defined (__SUPPORT_LD_DEBUG__)
	old_val = ((struct { unsigned long v; } __attribute__((packed)) *)
		   reloc_addr)->v;
#endif
	switch (reloc_type) {
	case R_C33_NONE:
		break;
	case R_C33_32:
		/* .eh_frame keeps pointers at any byte address.  */
		reloc_value = symbol_addr;
		if ((unsigned long) reloc_addr & 3)
			((struct { unsigned long v; } __attribute__((packed)) *)
			 reloc_addr)->v = reloc_value;
		else
			*reloc_addr = reloc_value;
		break;
	case R_C33_FUNCDESC_VALUE:
		funcval.entry_point = (void *) symbol_addr;
		funcval.got_value = symbol_addr
			? symbol_tpnt->loadaddr.got_value : 0;
		*(struct funcdesc_value volatile *) reloc_addr = funcval;
		reloc_value = symbol_addr;
		break;
	case R_C33_FUNCDESC:
		reloc_value = symbol_addr
			? (unsigned long) _dl_funcdesc_for((void *) symbol_addr,
				symbol_tpnt->loadaddr.got_value)
			: 0;
		*reloc_addr = reloc_value;
		break;
	default:
		return -1;
	}
#if defined (__SUPPORT_LD_DEBUG__)
	if (_dl_debug_reloc && _dl_debug_detail)
		_dl_dprintf(_dl_debug_file, "\tpatched: %x ==> %x @ %x\n",
			    old_val, reloc_value, reloc_addr);
#endif
	return 0;
}

/* There is no lazy binding: the linker puts every descriptor in
   DT_RELA, and no C33 module has a DT_JMPREL table.  */
void
_dl_parse_lazy_relocation_information(struct dyn_elf *rpnt,
	unsigned long rel_addr, unsigned long rel_size)
{
	if (rel_size)
		_dl_parse(rpnt->dyn, NULL, rel_addr, rel_size, _dl_do_reloc);
}

int
_dl_parse_relocation_information(struct dyn_elf *rpnt,
	struct r_scope_elem *scope, unsigned long rel_addr, unsigned long rel_size)
{
	return _dl_parse(rpnt->dyn, scope, rel_addr, rel_size, _dl_do_reloc);
}

#ifndef IS_IN_libdl
# include "../../libc/sysdeps/linux/c33/crtreloc.c"
#endif
