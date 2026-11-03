/* int 3, the C33's breakpoint: a SIGTRAP with TRAP_BRKPT and the PC at the
 * breakpoint itself, with or without a debugger.  Under ptrace, a tracer
 * reads the breakpoint back, writes another over the nop after it, and
 * resumes past both, as a debugger inserting and stepping over
 * breakpoints does. */
#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>

#define INT3 0x0483
#define NOP 0x0000

/* bp_site is called directly; under FDPIC its address as a function is
 * a descriptor's, so bp_addr names the code itself. */
void bp_site(void);
extern char bp_addr[];
__asm__(".text\n.global bp_site, bp_addr\n.type bp_site,@function\n"
	"bp_site:\nbp_addr:\n\tint 3\n\tnop\n\tret\n");

static volatile int trapped;

static void on_trap(int sig, siginfo_t *si, void *uc_)
{
	ucontext_t *uc = uc_;

	if (si->si_code == TRAP_BRKPT && si->si_addr == bp_addr &&
	    uc->uc_mcontext.pc == (unsigned long)bp_addr)
		trapped = 1;
	uc->uc_mcontext.pc += 2;
}

static int handled(void)
{
	struct sigaction sa = { .sa_sigaction = on_trap, .sa_flags = SA_SIGINFO };

	sigaction(SIGTRAP, &sa, 0);
	bp_site();
	signal(SIGTRAP, SIG_DFL);
	return trapped;
}

static int get_pc(pid_t pid, unsigned long *pc)
{
	unsigned long regs[24];
	struct iovec io = { regs, sizeof regs };

	if (ptrace(PTRACE_GETREGSET, pid, NT_PRSTATUS, &io))
		return -1;
	*pc = regs[23];
	return 0;
}

static int set_pc(pid_t pid, unsigned long pc)
{
	unsigned long regs[24];
	struct iovec io = { regs, sizeof regs };

	if (ptrace(PTRACE_GETREGSET, pid, NT_PRSTATUS, &io))
		return -1;
	regs[23] = pc;
	return ptrace(PTRACE_SETREGSET, pid, NT_PRSTATUS, &io);
}

static int stopped_by_trap(pid_t pid)
{
	int status;

	return waitpid(pid, &status, 0) == pid && WIFSTOPPED(status) &&
	       WSTOPSIG(status) == SIGTRAP;
}

static int traced(char *self)
{
	unsigned long pc, word;
	int status;
	pid_t pid = vfork();

	if (!pid) {
		ptrace(PTRACE_TRACEME, 0, 0, 0);
		execl(self, self, "child", (char *)0);
		_exit(127);
	}
	/* The exec's stop, then the breakpoint's. */
	if (pid < 0 || !stopped_by_trap(pid) || ptrace(PTRACE_CONT, pid, 0, 0) ||
	    !stopped_by_trap(pid) || get_pc(pid, &pc))
		return 0;
	errno = 0;
	word = ptrace(PTRACE_PEEKTEXT, pid, (void *)(pc & ~3UL), 0);
	if (errno || (word >> 8 * (pc & 3) & 0xffff) != INT3)
		return 0;
	/* A second breakpoint over the nop, and on to it. */
	word = ptrace(PTRACE_PEEKTEXT, pid, (void *)((pc + 2) & ~3UL), 0);
	unsigned long shift = 8 * ((pc + 2) & 3);
	unsigned long patched = (word & ~(0xffffUL << shift)) | (unsigned long)INT3 << shift;
	if (ptrace(PTRACE_POKETEXT, pid, (void *)((pc + 2) & ~3UL), (void *)patched) ||
	    set_pc(pid, pc + 2) || ptrace(PTRACE_CONT, pid, 0, 0) ||
	    !stopped_by_trap(pid))
		return 0;
	unsigned long pc2;
	if (get_pc(pid, &pc2) || pc2 != pc + 2)
		return 0;
	/* Put the nop back and run it to the end. */
	if (ptrace(PTRACE_POKETEXT, pid, (void *)((pc + 2) & ~3UL), (void *)word) ||
	    ptrace(PTRACE_CONT, pid, 0, 0) || waitpid(pid, &status, 0) != pid)
		return 0;
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

int main(int argc, char **argv)
{
	if (argc > 1) {
		bp_site();
		return 0;
	}
	int ok_handled = handled(), ok_traced = traced(argv[0]);
	printf("BREAKPOINT %s: handled %d, traced %d\n",
	       ok_handled && ok_traced ? "PASS" : "FAIL", ok_handled, ok_traced);
	return !(ok_handled && ok_traced);
}
