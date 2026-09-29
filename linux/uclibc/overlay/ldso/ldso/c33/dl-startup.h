/* C33 FDPIC dynamic linker startup.
 *
 * Licensed under the LGPL v2.1, see the file COPYING.LIB in this tarball.
 *
 * The kernel enters _start with the executable's load map in %r6, ours in
 * %r7 (zero if ld.so was run as a program), the dynamic section of the
 * interpreter in %r8, and argc at [%sp].  Nothing has been relocated, and
 * %r15 is not set, so the first thing is __self_reloc (crtreloc.c), which
 * uses neither: it relocates the words listed in .rofixup and returns the
 * last one, the address of our __dp, relocated -- our %r15.  A shared
 * object's list has only that entry; the rest of ld.so is relocated by
 * _dl_start from its own dynamic relocations.
 *
 * __ROFIXUP_LIST__ and __ROFIXUP_END__ are loaded as link-time addresses,
 * which __self_reloc translates through the load map: the text has no
 * PC-relative way to reach them.
 *
 * _dl_start returns the program's entry descriptor in the eight bytes at
 * dl_main_funcdesc.  The program's _start is entered with its load map in
 * %r6, the interpreter's in %r7, and in %r9 the descriptor of _dl_fini,
 * which __uClibc_main registers with atexit.
 */

__asm__(
"	.text\n"
"	.global	_start\n"
"	.type	_start,@function\n"
"	.hidden	_start\n"
"	.type	_dl_start,@function\n"
"_start:\n"
"	ld.w	%r0,%r6\n"
"	ld.w	%r1,%r7\n"
"	ld.w	%r2,%r8\n"
"	ld.w	%r3,%sp\n"
"	cmp	%r7,0\n"
"	jrne	1f\n"
"	ld.w	%r7,%r6\n"
"1:\n"
"	ld.w	%r6,%r7\n"
"	xld.w	%r7,__ROFIXUP_LIST__\n"
"	xld.w	%r8,__ROFIXUP_END__\n"
"	xcall	__self_reloc\n"
"	ld.w	%r15,%r4\n"
	/* A 16-byte-aligned frame: the descriptor _dl_start fills in at
	   [%sp+16], and its fifth and sixth arguments at [%sp] and [%sp+4].  */
"	ld.w	%r4,%sp\n"
"	xand	%r4,-16\n"
"	xsub	%r4,32\n"
"	ld.w	%sp,%r4\n"
"	xadd	%r4,16\n"
"	xld.w	[%sp],%r4\n"
"	xld.w	[%sp+4],%r3\n"
"	ld.w	%r6,%r15\n"
"	ld.w	%r7,%r0\n"
"	ld.w	%r8,%r1\n"
"	ld.w	%r9,%r2\n"
"	xcall	_dl_start\n"
"	ext	doff_hi(.L_dl_fini)\n"
"	ext	doff_lo(.L_dl_fini)\n"
"	ld.w	%r9,[%r15]\n"
"	xld.w	%r4,[%sp+16]\n"
"	xld.w	%r15,[%sp+20]\n"
"	ld.w	%sp,%r3\n"
"	ld.w	%r6,%r0\n"
"	ld.w	%r7,%r1\n"
"	ld.w	%r8,%r2\n"
"	jp	%r4\n"
"	.size	_start,.-_start\n"
	/* A descriptor for _dl_fini made by hand: ld.so relocates itself
	   before it can make canonical ones, and this one is only ever
	   called.  */
"	.section .data.rel.ro,\"aw\"\n"
"	.align	3\n"
".L_dl_fini_fd:\n"
"	.long	_dl_fini\n"
"	.long	__dp\n"
".L_dl_fini:\n"
"	.long	.L_dl_fini_fd\n"
"	.previous\n"
);

#undef DL_START
#define DL_START(X)   \
static void  __attribute__ ((used)) \
_dl_start (Elf32_Addr dl_boot_got_pointer, \
	   struct elf32_fdpic_loadmap *dl_boot_progmap, \
	   struct elf32_fdpic_loadmap *dl_boot_ldsomap, \
	   Elf32_Dyn *dl_boot_ldso_dyn_pointer, \
	   struct funcdesc_value *dl_main_funcdesc, \
	   X)

/* ARGS is the address of argc.  */
#define GET_ARGV(ARGVP, ARGS) ARGVP = (((unsigned long *)ARGS) + 1)

/* ld.so is linked -Bsymbolic, so its relocations are against itself: an
   address (R_C33_32 without a symbol, the link-time address in the addend)
   or a descriptor for one of its functions.  SYM is null for those; one
   naming a symbol can only be an undefined weak one, which is zero.  */
#define C33_BOOT_VALUE(RELP, SYMBOL, LOAD, SYM)				\
	((SYM) == NULL							\
	 ? (unsigned long) DL_RELOC_ADDR((LOAD), (RELP)->r_addend)	\
	 : (SYM)->st_shndx == SHN_UNDEF ? 0				\
	 : (SYMBOL) + (RELP)->r_addend)

#define PERFORM_BOOTSTRAP_RELOC(RELP,REL,SYMBOL,LOAD,SYM)		\
	switch (ELF_R_TYPE((RELP)->r_info)) {				\
	case R_C33_NONE:						\
		break;							\
	case R_C33_32:							\
		*(REL) = C33_BOOT_VALUE(RELP, SYMBOL, LOAD, SYM);	\
		break;							\
	case R_C33_FUNCDESC_VALUE:					\
	  {								\
		struct funcdesc_value fv = {				\
			(void *) C33_BOOT_VALUE(RELP, SYMBOL, LOAD, SYM), \
			(LOAD).got_value				\
		};							\
		*(struct funcdesc_value volatile *)(REL) = fv;		\
		break;							\
	  }								\
	default:							\
		_dl_exit(1);						\
	}

/*
 * Transfer control to the user's application, once the dynamic loader
 * is done: fill in the descriptor _start jumps through.
 */
#define START()	do {							\
  struct elf_resolve *exec_mod = _dl_loaded_modules;			\
  dl_main_funcdesc->entry_point = _dl_elf_main;				\
  while (exec_mod->libtype != elf_executable)				\
    exec_mod = exec_mod->next;						\
  dl_main_funcdesc->got_value = exec_mod->loadaddr.got_value;		\
  return;								\
} while (0)
