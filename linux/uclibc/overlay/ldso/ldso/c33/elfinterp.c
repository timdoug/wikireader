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
   one pass over a module, each symbol is looked up once.  Few symbols are
   named (libc's relocations name 59 of its 1,687), so the cache is a small
   open-addressed table on the stack: an array by symbol index would be
   13 KB for libc, which ld.so's allocator cannot give back.  An entry
   holds the symbol's value, which for TLS is a module offset (libc's
   errno is zero), and its defining module.  Insertion stops at three
   quarters, so a probe always ends.  */
#define C33_SYM_CACHE_SIZE	256

struct c33_sym_cache {
	unsigned long idx;		/* 0: free */
	char *addr;
	struct elf_resolve *tpnt;
};

struct c33_reloc_ctx {
	struct elf_resolve *tpnt;
	struct r_scope_elem *scope;
	ElfW(Sym) *symtab;
	char *strtab;
	struct c33_segs segs;
	unsigned int free;
	struct c33_sym_cache cache[C33_SYM_CACHE_SIZE];
};

static __always_inline struct c33_sym_cache *
c33_cache_find(struct c33_sym_cache *cache, unsigned long idx)
{
	unsigned long slot;
	struct c33_sym_cache *e;

	for (slot = idx;; slot++) {
		e = &cache[slot & (C33_SYM_CACHE_SIZE - 1)];
		if (e->idx == idx || e->idx == 0)
			return e;
	}
}

static __always_inline void
c33_cache_add(struct c33_reloc_ctx *ctx, unsigned long idx, char *addr,
	      struct elf_resolve *def)
{
	struct c33_sym_cache *e = c33_cache_find(ctx->cache, idx);

	if (e->idx || !ctx->free)
		return;
	ctx->free--;
	e->idx = idx;
	e->addr = addr;
	e->tpnt = def;
}

static __always_inline void
c33_store(unsigned long addr, unsigned long value)
{
	/* .eh_frame keeps pointers at any byte address.  */
	if (addr & 3)
		((struct { unsigned long v; } __attribute__((packed)) *)
		 addr)->v = value;
	else
		*(unsigned long *) addr = value;
}

/* The relocations whose symbol is cached, or which have none, without a
   call, so that the loop's state stays in registers; tests in order of
   frequency, and no jump table, whose base would take one.  Returns the
   first relocation it leaves to c33_do_reloc.  */
static const ELF_RELOC * __attribute__((noinline, optimize("no-jump-tables")))
c33_relocate_fast(struct c33_reloc_ctx *ctx, const ELF_RELOC *rpnt,
		  const ELF_RELOC *end)
{
	unsigned long split = ctx->segs.split;
	unsigned long delta0 = ctx->segs.delta0, delta1 = ctx->segs.delta1;

#if defined (__SUPPORT_LD_DEBUG__)
	return rpnt;
#endif
	if (!ctx->segs.two)
		return rpnt;
	for (; rpnt < end; rpnt++) {
		unsigned long info = rpnt->r_info;
		unsigned long type = ELF_R_TYPE(info);
		unsigned long off = rpnt->r_offset;
		unsigned long value = rpnt->r_addend;
		struct elf_resolve *def;

		off += off >= split ? delta1 : delta0;
		if (ELF_R_SYM(info)) {
			struct c33_sym_cache *e =
				c33_cache_find(ctx->cache, ELF_R_SYM(info));

			if (e->idx != ELF_R_SYM(info))
				break;
			def = e->tpnt;
			value += (unsigned long) e->addr;
		} else {
			/* R_C33_32 and TLS take the addend as it is; the
			   others hold the module's own address there.  */
			def = ctx->tpnt;
			if (type == R_C33_RELATIVE
			    || type == R_C33_FUNCDESC_VALUE)
				value += value >= split ? delta1 : delta0;
		}

		if (type == R_C33_32 || type == R_C33_RELATIVE) {
			c33_store(off, value);
#ifdef __UCLIBC_HAS_TLS__
		} else if (type == R_C33_TLS_TPREL32) {
			if (def->l_tls_offset == NO_TLS_OFFSET)
				break;
			*(unsigned long *) off = def->l_tls_offset + value;
#endif
		} else if (type == R_C33_FUNCDESC_VALUE) {
			struct funcdesc_value volatile *fd = (void *) off;

			fd->entry_point = (void *) value;
			fd->got_value = value ? def->loadaddr.got_value : 0;
#ifdef __UCLIBC_HAS_TLS__
		} else if (type == R_C33_TLS_DTPREL32) {
			*(unsigned long *) off = value;
		} else if (type == R_C33_TLS_DTPMOD32) {
			*(unsigned long *) off = def->l_tls_modid;
#endif
		} else if (type != R_C33_NONE) {
			/* R_C33_FUNCDESC, which needs the descriptor table,
			   and anything not understood.  */
			break;
		}
	}
	return rpnt;
}

/* Any relocation, looking its symbol up and caching it.  */
static int __attribute__((noinline))
c33_do_reloc(struct c33_reloc_ctx *ctx, const ELF_RELOC *rpnt)
{
	struct elf_resolve *tpnt = ctx->tpnt;
	ElfW(Sym) *symtab = ctx->symtab;
	int reloc_type;
	int symtab_index;
	char *symname;
	unsigned long reloc_addr;
	unsigned long reloc_value = 0;
	unsigned long symbol_addr = 0;
	struct elf_resolve *symbol_tpnt = tpnt;
	struct c33_sym_cache *e;
	struct symbol_ref sym_ref;
#if defined (__SUPPORT_LD_DEBUG__)
	unsigned long old_val;
#endif

	reloc_addr = c33_segs_addr(&ctx->segs, tpnt->loadaddr.map,
				   rpnt->r_offset);
	reloc_type = ELF_R_TYPE(rpnt->r_info);
	symtab_index = ELF_R_SYM(rpnt->r_info);
	sym_ref.sym = &symtab[symtab_index];
	sym_ref.tpnt = NULL;
	symname = ctx->strtab + symtab[symtab_index].st_name;
	e = NULL;
	if (symtab_index) {
		e = c33_cache_find(ctx->cache, symtab_index);
		if (e->idx != (unsigned long) symtab_index)
			e = NULL;
	}

#ifdef __UCLIBC_HAS_TLS__
	if (c33_tls_reloc_p(reloc_type)) {
		/* TLS symbol values are offsets, including the valid offset zero.
		 * They must never be translated through an FDPIC load map. */
		unsigned long offset = rpnt->r_addend;
		if (symtab_index) {
			if (e) {
				offset += (unsigned long) e->addr;
				symbol_tpnt = e->tpnt;
			} else if (ELF_ST_BIND(sym_ref.sym->st_info) == STB_LOCAL) {
				offset += sym_ref.sym->st_value;
				c33_cache_add(ctx, symtab_index,
					      (char *) sym_ref.sym->st_value, tpnt);
			} else {
				unsigned long value = (unsigned long)_dl_find_hash(symname,
					ctx->scope, tpnt, ELF_RTYPE_CLASS_PLT, &sym_ref);
				if (!sym_ref.tpnt || sym_ref.sym->st_shndx == SHN_UNDEF ||
				    ELF_ST_TYPE(sym_ref.sym->st_info) != STT_TLS)
					return 1;
				symbol_tpnt = sym_ref.tpnt;
				offset += value;
				c33_cache_add(ctx, symtab_index, (char *) value,
					      symbol_tpnt);
			}
		}
		switch (reloc_type) {
		case R_C33_TLS_DTPMOD32:
			*(unsigned long *) reloc_addr = symbol_tpnt->l_tls_modid;
			break;
		case R_C33_TLS_DTPREL32:
			*(unsigned long *) reloc_addr = offset;
			break;
		case R_C33_TLS_TPREL32:
			CHECK_STATIC_TLS((struct link_map *)symbol_tpnt);
			*(unsigned long *) reloc_addr = symbol_tpnt->l_tls_offset + offset;
			break;
		}
		return 0;
	}
#endif

	if (symtab_index == 0) {
		/* R_C33_32 without a symbol is its addend; the others hold
		   the module's own address there.  */
		symbol_addr = reloc_type == R_C33_32 ? rpnt->r_addend
			: c33_segs_addr(&ctx->segs, tpnt->loadaddr.map,
					rpnt->r_addend);
	} else if (e) {
		symbol_addr = (unsigned long) e->addr + rpnt->r_addend;
		symbol_tpnt = e->tpnt;
	} else if (ELF_ST_BIND(symtab[symtab_index].st_info) == STB_LOCAL) {
		symbol_addr = c33_segs_addr(&ctx->segs, tpnt->loadaddr.map,
					    symtab[symtab_index].st_value);
		c33_cache_add(ctx, symtab_index, (char *) symbol_addr, tpnt);
		symbol_addr += rpnt->r_addend;
	} else {
		symbol_addr = (unsigned long)
			_dl_find_hash(symname, ctx->scope, tpnt,
				elf_machine_type_class(reloc_type), &sym_ref);
		/* Undefined weak references are allowed, and are zero.  */
		if (!symbol_addr
		    && ELF_ST_BIND(symtab[symtab_index].st_info) != STB_WEAK) {
			_dl_dprintf(2, "%s: can't resolve symbol '%s'\n",
				    _dl_progname, symname);
			_dl_exit(1);
		}
		symbol_tpnt = sym_ref.tpnt;
		if (symbol_addr) {
			c33_cache_add(ctx, symtab_index, (char *) symbol_addr,
				      symbol_tpnt);
			symbol_addr += rpnt->r_addend;
		}
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
		reloc_value = symbol_addr;
		c33_store(reloc_addr, reloc_value);
		break;
	case R_C33_FUNCDESC_VALUE: {
		struct funcdesc_value funcval;

		funcval.entry_point = (void *) symbol_addr;
		funcval.got_value = symbol_addr
			? symbol_tpnt->loadaddr.got_value : 0;
		*(struct funcdesc_value volatile *) reloc_addr = funcval;
		reloc_value = symbol_addr;
		break;
	}
	case R_C33_FUNCDESC:
		reloc_value = symbol_addr
			? (unsigned long) _dl_funcdesc_for((void *) symbol_addr,
				symbol_tpnt->loadaddr.got_value)
			: 0;
		/* Personality pointers in .eh_frame need not be word-aligned. */
		c33_store(reloc_addr, reloc_value);
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

static void __attribute__((noinline, noreturn))
c33_bad_reloc(const ELF_RELOC *rpnt, ElfW(Sym) *symtab, char *strtab)
{
	int symtab_index = ELF_R_SYM(rpnt->r_info);

	_dl_dprintf(2, "\n%s: ", _dl_progname);
	if (symtab_index)
		_dl_dprintf(2, "symbol '%s': ",
			    strtab + symtab[symtab_index].st_name);
	_dl_dprintf(2, "can't handle reloc type %x\n", ELF_R_TYPE(rpnt->r_info));
	_dl_exit(1);
}

/* Lazy binding.  A .plt descriptor starts out as {the link-time address
   of its entry's stub, 0}; loading the module makes that {the stub, the
   module's %r15}.  */
void
_dl_parse_lazy_relocation_information(struct dyn_elf *rpnt,
	unsigned long rel_addr, unsigned long rel_size)
{
	struct elf_resolve *tpnt = rpnt->dyn;
	const ELF_RELOC *r = (const ELF_RELOC *) rel_addr;
	const ELF_RELOC *end = r + rel_size / sizeof(ELF_RELOC);
	struct elf32_fdpic_loadmap *map = tpnt->loadaddr.map;
	void *got = tpnt->loadaddr.got_value;
	struct c33_segs segs;

	c33_segs_init(&segs, map);
	for (; r < end; r++) {
		struct funcdesc_value volatile *fd;

		if (ELF_R_TYPE(r->r_info) != R_C33_FUNCDESC_VALUE) {
			if (ELF_R_TYPE(r->r_info) == R_C33_NONE)
				continue;
			c33_bad_reloc(r, (ElfW(Sym) *) tpnt->dynamic_info[DT_SYMTAB],
				      (char *) tpnt->dynamic_info[DT_STRTAB]);
		}
		fd = (void *) c33_segs_addr(&segs, map, r->r_offset);
		fd->entry_point = (void *) c33_segs_addr(&segs, map,
			(unsigned long) fd->entry_point);
		fd->got_value = got;
	}
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
				 tpnt, ELF_RTYPE_CLASS_PLT, &sym_ref);
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
	struct c33_reloc_ctx ctx;
	const ELF_RELOC *r = (const ELF_RELOC *) rel_addr;
	const ELF_RELOC *end = r + rel_size / sizeof(ELF_RELOC);
	int i;

	ctx.tpnt = rpnt->dyn;
	ctx.scope = scope;
	ctx.symtab = (ElfW(Sym) *) ctx.tpnt->dynamic_info[DT_SYMTAB];
	ctx.strtab = (char *) ctx.tpnt->dynamic_info[DT_STRTAB];
	c33_segs_init(&ctx.segs, ctx.tpnt->loadaddr.map);
	ctx.free = C33_SYM_CACHE_SIZE / 4 * 3;
	for (i = 0; i < C33_SYM_CACHE_SIZE; i++)
		ctx.cache[i].idx = 0;

	while ((r = c33_relocate_fast(&ctx, r, end)) < end) {
		int res;

		debug_sym(ctx.symtab, ctx.strtab, ELF_R_SYM(r->r_info));
		debug_reloc(ctx.symtab, ctx.strtab, r);
		res = c33_do_reloc(&ctx, r);
		if (res < 0)
			c33_bad_reloc(r, ctx.symtab, ctx.strtab);
		if (res > 0) {
			_dl_dprintf(2, "\n%s: ", _dl_progname);
			if (ELF_R_SYM(r->r_info))
				_dl_dprintf(2, "symbol '%s': ", ctx.strtab
					    + ctx.symtab[ELF_R_SYM(r->r_info)].st_name);
			_dl_dprintf(2, "can't resolve symbol\n");
			return 1;
		}
		r++;
	}
	return 0;
}

#ifndef IS_IN_libdl
# include "../../libc/sysdeps/linux/c33/crtreloc.c"
#endif
