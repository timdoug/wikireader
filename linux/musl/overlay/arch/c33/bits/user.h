/* The kernel's struct pt_regs, which a core dump's registers are. */
struct user_regs_struct {
	unsigned long r[16];
	unsigned long alr, ahr, sp, vector, orig_r4, reserved, psr, pc;
};

#define ELF_NGREG 24
typedef unsigned long elf_greg_t, elf_gregset_t[ELF_NGREG];
typedef struct { unsigned long __unused; } elf_fpregset_t;
