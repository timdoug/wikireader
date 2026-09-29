/* C33 FDPIC shared library loader support.
 *
 * Licensed under the LGPL v2.1, see the file COPYING.LIB in this tarball.
 *
 * Modelled on the FR-V loader.  Relocations are RELA; .plt descriptors
 * are bound at their first call, the rest when the module is loaded:
 *
 *   R_C33_RELATIVE	a word: the module's own link-time address in the
 *			addend, relocated through its load map (most come
 *			first, counted by DT_RELACOUNT: elf_machine_relative);
 *   R_C33_32		a word: SYM + addend;
 *   R_C33_FUNCDESC	a word: the address of SYM's canonical descriptor;
 *   R_C33_FUNCDESC_VALUE
 *			a .got descriptor, {SYM, SYM's module's %r15}, or with
 *			no symbol, {the addend relocated, this module's}.
 */

#include <sys/cdefs.h>
#include <features.h>

/* A module's relocations name the same few symbols again and again: every
   function built with the stack protector has its own pool word for
   __stack_chk_guard, and a lookup costs thousands of cycles here.  During
   one pass over a module, each symbol is looked up once.  */
struct c33_sym_cache {
	char *addr;
	struct elf_resolve *tpnt;
};
static struct c33_sym_cache *_dl_c33_sym_cache;

static int _dl_do_reloc(struct elf_resolve *tpnt, struct r_scope_elem *scope,
			ELF_RELOC *rpnt, ElfW(Sym) *symtab, char *strtab);

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

	if (reloc_fnc == _dl_do_reloc) {
		unsigned long nsyms = 0;

		for (i = 0; i < rel_size; i++)
			if (ELF_R_SYM(rpnt[i].r_info) >= nsyms)
				nsyms = ELF_R_SYM(rpnt[i].r_info) + 1;
		_dl_c33_sym_cache = nsyms
			? _dl_malloc(nsyms * sizeof(*_dl_c33_sym_cache)) : NULL;
		if (_dl_c33_sym_cache)
			_dl_memset(_dl_c33_sym_cache, 0,
				   nsyms * sizeof(*_dl_c33_sym_cache));
	}

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
			break;
		}
	}
	if (_dl_c33_sym_cache) {
		_dl_free(_dl_c33_sym_cache);
		_dl_c33_sym_cache = NULL;
	}
	return i < rel_size ? 1 : 0;
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
		/* R_C33_32 without a symbol is its addend; the others hold
		   the module's own address there.  */
		symbol_addr = reloc_type == R_C33_32 ? rpnt->r_addend
			: (unsigned long) DL_RELOC_ADDR(tpnt->loadaddr,
							rpnt->r_addend);
	} else if (ELF_ST_BIND(symtab[symtab_index].st_info) == STB_LOCAL) {
		symbol_addr = (unsigned long) DL_RELOC_ADDR(tpnt->loadaddr,
			symtab[symtab_index].st_value) + rpnt->r_addend;
	} else if (_dl_c33_sym_cache && _dl_c33_sym_cache[symtab_index].addr) {
		symbol_addr = (unsigned long) _dl_c33_sym_cache[symtab_index].addr
			+ rpnt->r_addend;
		symbol_tpnt = _dl_c33_sym_cache[symtab_index].tpnt;
	} else {
		symbol_addr = (unsigned long)
			_dl_find_hash(symname, scope, NULL, 0, &sym_ref);
		if (symbol_addr && _dl_c33_sym_cache) {
			_dl_c33_sym_cache[symtab_index].addr = (char *) symbol_addr;
			_dl_c33_sym_cache[symtab_index].tpnt = sym_ref.tpnt;
		}
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
	case R_C33_RELATIVE:
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

/* Lazy binding.  A .plt descriptor starts out as {the link-time address
   of its entry's stub, 0}; loading the module makes that {the stub, the
   module's %r15}.  */
static int
_dl_do_lazy_reloc(struct elf_resolve *tpnt,
		  struct r_scope_elem *scope __attribute__((unused)),
		  ELF_RELOC *rpnt, ElfW(Sym) *symtab __attribute__((unused)),
		  char *strtab __attribute__((unused)))
{
	struct funcdesc_value volatile *fd = (struct funcdesc_value *)
		DL_RELOC_ADDR(tpnt->loadaddr, rpnt->r_offset);

	switch (ELF_R_TYPE(rpnt->r_info)) {
	case R_C33_NONE:
		break;
	case R_C33_FUNCDESC_VALUE:
		fd->entry_point = (void *) DL_RELOC_ADDR(tpnt->loadaddr,
							 fd->entry_point);
		fd->got_value = tpnt->loadaddr.got_value;
		break;
	default:
		return -1;
	}
	return 0;
}

void
_dl_parse_lazy_relocation_information(struct dyn_elf *rpnt,
	unsigned long rel_addr, unsigned long rel_size)
{
	_dl_parse(rpnt->dyn, NULL, rel_addr, rel_size, _dl_do_lazy_reloc);
}

/* The first call through a .plt descriptor (resolve.S).  STUB is an address
   in the calling module's .plt, RELOC_OFFSET the entry's offset in its
   .rela.plt.  The descriptor's %r15 is written before its entry point: a
   caller reading the two words while this runs gets the stub, which works
   whatever %r15 it read, or the whole new descriptor.  */
struct funcdesc_value volatile attribute_hidden *
_dl_linux_resolver(void *stub, unsigned long reloc_offset)
{
	struct elf_resolve *tpnt;
	ELF_RELOC *this_reloc;
	ElfW(Sym) *symtab;
	char *strtab, *symname, *new_addr;
	struct funcdesc_value volatile *got_entry;
	struct symbol_ref sym_ref;

	for (tpnt = _dl_loaded_modules; tpnt; tpnt = tpnt->next)
		if (__dl_addr_in_loadaddr(stub, tpnt->loadaddr))
			break;
	if (!tpnt) {
		_dl_dprintf(2, "%s: lazy call from unknown code at %x\n",
			    _dl_progname, stub);
		_dl_exit(1);
	}

	this_reloc = (ELF_RELOC *)(tpnt->dynamic_info[DT_JMPREL] + reloc_offset);
	symtab = (ElfW(Sym) *) tpnt->dynamic_info[DT_SYMTAB];
	strtab = (char *) tpnt->dynamic_info[DT_STRTAB];
	sym_ref.sym = &symtab[ELF_R_SYM(this_reloc->r_info)];
	sym_ref.tpnt = NULL;
	symname = strtab + sym_ref.sym->st_name;
	got_entry = (struct funcdesc_value *)
		DL_RELOC_ADDR(tpnt->loadaddr, this_reloc->r_offset);

	new_addr = _dl_find_hash(symname, &_dl_loaded_modules->symbol_scope,
				 NULL, 0, &sym_ref);
	if (!new_addr) {
		_dl_dprintf(2, "%s: can't resolve symbol '%s'\n",
			    _dl_progname, symname);
		_dl_exit(1);
	}

	got_entry->got_value = sym_ref.tpnt->loadaddr.got_value;
	got_entry->entry_point = new_addr;
#if defined (__SUPPORT_LD_DEBUG__)
	if (_dl_debug_bindings)
		_dl_dprintf(_dl_debug_file, "\nresolve function: %s", symname);
#endif
	return got_entry;
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
