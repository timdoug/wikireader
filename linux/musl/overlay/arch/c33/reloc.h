/* C33 FDPIC: each module, the program and every shared library, has a text
 * segment shared between processes and a data segment of its own, placed
 * independently; its code reaches the data through %r15, the address of
 * its __dp, where its .got starts.  A function pointer is the address of a
 * descriptor, {entry point, %r15}.  The ABI is in the C33 toolchain's
 * gcc/ABI.md. */

#define LDSO_ARCH "c33"

#define TPOFF_K 0

#define R_C33_32		1
#define R_C33_FUNCDESC		32
#define R_C33_FUNCDESC_VALUE	33
#define R_C33_RELATIVE		34
#define R_C33_TLS_DTPMOD32	35
#define R_C33_TLS_DTPREL32	36
#define R_C33_TLS_TPREL32	37

#define REL_SYMBOLIC    R_C33_32
#define REL_RELATIVE    R_C33_RELATIVE
#define REL_DTPMOD      R_C33_TLS_DTPMOD32
#define REL_DTPOFF      R_C33_TLS_DTPREL32
#define REL_TPOFF       R_C33_TLS_TPREL32
#define REL_FUNCDESC    R_C33_FUNCDESC
#define REL_FUNCDESC_VAL R_C33_FUNCDESC_VALUE

#define DL_FDPIC 1
#define DL_NOMMU_SUPPORT 1

/* A module's own addresses are R_C33_RELATIVE, and descriptors for its
 * own functions R_C33_FUNCDESC_VALUE without a symbol; both carry the
 * link-time address in the addend, which the load map translates. */
#define FDPIC_ADDEND_RELATIVE 1
#define IS_RELATIVE(x,s) ( (R_TYPE(x) == REL_RELATIVE) || \
	(R_TYPE(x) == REL_FUNCDESC_VAL && !R_SYM(x)) )

/* .init_array and .fini_array hold entry points, which are called with
 * their module's %r15, not descriptors. */
#define FDPIC_CTOR_ENTRY_POINTS 1

/* The program's start code saves its load map here, for the static
 * library's TLS and dl_iterate_phdr. */
#define FDPIC_EXEC_LOADMAP __c33_exec_loadmap

/* Enter the program with its load map in %r6 and ours in %r7, both of
 * which _dlstart left below the stack vector. */
#define CRTJMP(pc,sp) do { \
	register size_t r6 __asm__("r6") = ((size_t *)(sp))[-2]; \
	register size_t r7 __asm__("r7") = ((size_t *)(sp))[-1]; \
	__asm__ __volatile__( "ld.w %%sp,%1\n\tjp %0" \
	: : "r"(pc), "r"(sp), "r"(r6), "r"(r7) : "memory" ); } while(0)

/* This module's %r15, for calling an entry point of its own. */
#define FDPIC_SELF_GOT() ({ unsigned long __got; \
	__asm__ ("ld.w %0,%%r15" : "=r"(__got)); __got; })
