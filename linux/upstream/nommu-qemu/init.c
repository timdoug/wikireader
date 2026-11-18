/*
 * /init for the no-MMU reproducers. The kernel command line names one test
 * after "--"; init runs it, prints TEST-DONE and powers off.
 *
 *   bigmap        8 MiB private anonymous mmap (fixes/0045)
 *   traced-map    read-only private mmap of a ramfs file under ptrace (0042)
 *   bigenv        exec with a 112 KiB environment into an 8 KiB stack (0031)
 *   smallenv      exec with a 12 KB environment into an 8 KiB stack (0031)
 *   sweep         execs across stack sizes and argument layouts (0031)
 *   stack         smallenv, sweep and bigenv in one boot (0031)
 *   devmem        /dev/mem mmap of the kernel image's first page (0032)
 *   devmem-alias  /dev/mem mmap at the start of another process's anonymous
 *                 mapping (0033)
 */
#include "sys.h"

extern long spawn(int (*fn)(void), void *stack_top);

static char child_stack[16384] __attribute__((aligned(16)));
#define CHILD_STACK (child_stack + sizeof(child_stack))

static int str_eq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

static int mem_eq(const char *a, const char *b, unsigned long n)
{
	while (n--)
		if (*a++ != *b++)
			return 0;
	return 1;
}

static long wait_child(long pid)
{
	int status = 0;
	long r = sys(__NR_wait4, pid, &status, 0, 0, 0, 0);

	put("child ");
	put_dec(pid);
	put(" wait4 ");
	put_result(r);
	put(", status ");
	put_hex(status);
	put("\n");
	return status;
}

/* fixes/0045 */
static void test_bigmap(void)
{
	long p = sys_mmap(0, 8UL << 20, PROT_READ | PROT_WRITE,
			  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	put("mmap(8 MiB, MAP_PRIVATE | MAP_ANONYMOUS): ");
	put_result(p);
	put("\n");
}

/* fixes/0042 */
static int traced_map_child(void)
{
	long fd, p, r;

	r = sys(__NR_ptrace, PTRACE_TRACEME, 0, 0, 0, 0, 0);
	put("ptrace(PTRACE_TRACEME): ");
	put_result(r);
	put("\n");
	fd = sys_open("/data", O_RDONLY);
	p = sys_mmap(0, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
	put("mmap(/data, PROT_READ, MAP_PRIVATE) while traced: ");
	put_result(p);
	put("\n");
	if (!is_err(p)) {
		put(mem_eq((const char *)p, "ramfs data", 10) ?
		    "mapping matches the file\n" :
		    "mapping does NOT match the file\n");
		sys(__NR_munmap, p, 4096, 0, 0, 0, 0);
	}
	return 0;
}

static void test_traced_map(void)
{
	wait_child(spawn(traced_map_child, CHILD_STACK));
}

/*
 * fixes/0031. An exec's argv and envp point at one argument string and one
 * environment string, repeated: the kernel copies each element on its own,
 * so the copies count in full against the argument limit and the stack
 * image, and the arrays alone reach several hundred KiB.
 */
#define MAX_VEC 65536
#define MAX_STRING 32768
static char arg_string[MAX_STRING + 1], env_string[MAX_STRING + 1];
static char *exec_argv[MAX_VEC + 3], *exec_envp[MAX_VEC + 1];
static const char *exec_path;
static char size_arg[12];

static void fill(char *buf, int len, char first)
{
	int i;

	for (i = 0; i < len - 1; i++)
		buf[i] = i == 0 ? first : i == 1 && len > 2 ? '=' : 'x';
	buf[len > 0 ? len - 1 : 0] = 0;
}

/*
 * Lay out an exec of exec_path: argv[0], then size_arg if with_size, then
 * nargs copies of an arglen-byte string; envp holds nenv copies of an
 * envlen-byte string. Lengths count the terminating NUL.
 */
static void layout(int with_size, int nargs, int arglen, int nenv,
		   int envlen)
{
	int i, n = 0;

	fill(arg_string, arglen, 'a');
	fill(env_string, envlen, 'E');
	exec_argv[n++] = (char *)exec_path;
	if (with_size)
		exec_argv[n++] = size_arg;
	for (i = 0; i < nargs; i++)
		exec_argv[n++] = arg_string;
	exec_argv[n] = 0;
	for (i = 0; i < nenv; i++)
		exec_envp[i] = env_string;
	exec_envp[nenv] = 0;
}

/* Returns 64 + errno when execve fails. */
static int exec_child(void)
{
	long r = sys(__NR_execve, exec_path, exec_argv, exec_envp, 0, 0, 0);

	return 64 + (int)-r;
}

static void test_env(int strings, int len)
{
	exec_path = "/stack-victim";
	layout(0, 0, 0, strings, len);
	put("execve(/stack-victim) with ");
	put_dec(strings);
	put(" environment strings of ");
	put_dec(len);
	put(" bytes\n");
	wait_child(spawn(exec_child, CHILD_STACK));
}

static void set_size_arg(unsigned long v)
{
	char tmp[12];
	int i = 0, j = 0;

	do {
		tmp[i++] = '0' + v % 10;
		v /= 10;
	} while (v);
	while (i)
		size_arg[j++] = tmp[--i];
	size_arg[j] = 0;
}

static int sweep_ok, sweep_e2big, sweep_failed;

static void sweep_case(unsigned long size, int nargs, int arglen, int nenv,
		       int envlen)
{
	int status = 0;
	long pid, code;

	layout(1, nargs, arglen, nenv, envlen);
	pid = spawn(exec_child, CHILD_STACK);
	sys(__NR_wait4, pid, &status, 0, 0, 0, 0);
	put("sweep stack ");
	put_dec(size);
	put(" args ");
	put_dec(nargs);
	put("x");
	put_dec(arglen);
	put(" env ");
	put_dec(nenv);
	put("x");
	put_dec(envlen);
	put(": ");
	if (status & 0x7f) {
		put("killed by signal ");
		put_dec(status & 0x7f);
		sweep_failed++;
	} else {
		code = (status >> 8) & 0xff;
		if (code == 0) {
			put("ok");
			sweep_ok++;
		} else if (code == 64 + 7) {
			put("E2BIG");
			sweep_e2big++;
		} else {
			put(code == 1 ? "FAIL: sp below the stack mapping" :
			    code == 2 ? "FAIL: strings outside every mapping" :
			    code == 3 ? "FAIL: less room than promised" :
			    "FAIL: exit status ");
			if (code > 3)
				put_dec(code);
			sweep_failed++;
		}
	}
	put("\n");
}

static void test_sweep(void)
{
	int v, c;

	for (v = 0; v < 3; v++) {
		unsigned long size;

		if (v == 0) {
			exec_path = "/stack-victim";
			size = 8192;
		} else if (v == 1) {
			exec_path = "/stack-victim-64k";
			size = 65536;
		} else {
			exec_path = "/stack-victim-default";
			size = 0;
		}
		set_size_arg(size);
		/* Environment strings of growing size, up to the 128 KiB limit. */
		for (c = 0; c < 12; c++) {
			int n = c < 6 ? 1 : 4;
			int len = c == 0 ? 2 : c == 1 ? 1000 : c == 2 ? 2000 :
				  c == 3 ? 4000 : c == 4 ? 8000 : c == 5 ? 16000 :
				  c == 6 ? 8000 : c == 7 ? 16000 : c == 8 ? 24000 :
				  c == 9 ? 30000 : c == 10 ? 32700 : 32760;

			sweep_case(size, 0, 0, n, len);
		}
		/* Many small strings: the argv and envp arrays dominate. */
		for (c = 0; c < 4; c++) {
			int n = c == 0 ? 1000 : c == 1 ? 4000 : c == 2 ? 16000 :
				60000;

			sweep_case(size, 0, 0, n, 2);
			sweep_case(size, n, 2, 0, 0);
		}
	}
	put("SWEEP-SUMMARY: ");
	put_dec(sweep_ok);
	put(" ok, ");
	put_dec(sweep_e2big);
	put(" E2BIG, ");
	put_dec(sweep_failed);
	put(" failed\n");
}

/* fixes/0032: the RISC-V kernel Image header contains "RISCV" at offset 0x30. */
static void test_devmem(void)
{
	long fd = sys_open("/dev/mem", O_RDONLY | O_SYNC);
	long p;

	put("open(/dev/mem): ");
	put_result(fd);
	put("\n");
	p = sys_mmap(0, 4096, PROT_READ, MAP_SHARED, fd, 0x80000000UL);
	put("mmap(/dev/mem, 4096, offset 0x80000000): ");
	put_result(p);
	put("\n");
	if (!is_err(p))
		put(mem_eq((const char *)p + 0x30, "RISCV", 5) ?
		    "mapping matches the kernel Image header\n" :
		    "mapping does NOT match the kernel Image header\n");
}

/* fixes/0033 */
static long to_parent[2], from_parent[2];

static int holder_child(void)
{
	char *argv[2];
	char *envp[1];

	sys(__NR_dup3, to_parent[1], 4, 0, 0, 0, 0);
	sys(__NR_dup3, from_parent[0], 3, 0, 0, 0, 0);
	argv[0] = (char *)"/holder";
	argv[1] = 0;
	envp[0] = 0;
	sys(__NR_execve, argv[0], argv, envp, 0, 0, 0);
	return 1;
}

static void test_devmem_alias(void)
{
	int fds[2];
	unsigned long addr = 0;
	long pid, fd, p, r;

	sys(__NR_pipe2, fds, 0, 0, 0, 0, 0);
	to_parent[0] = fds[0];
	to_parent[1] = fds[1];
	sys(__NR_pipe2, fds, 0, 0, 0, 0, 0);
	from_parent[0] = fds[0];
	from_parent[1] = fds[1];
	pid = spawn(holder_child, CHILD_STACK);
	r = sys(__NR_read, to_parent[0], &addr, sizeof(addr), 0, 0, 0);
	if (r != sizeof(addr)) {
		put("reading /holder's address failed\n");
		return;
	}
	put("/holder's anonymous page is at ");
	put_hex(addr);
	put("\n");

	fd = sys_open("/dev/mem", O_RDWR | O_SYNC);
	p = sys_mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, addr);
	put("mmap(/dev/mem, 4096, offset of that page): ");
	put_result(p);
	put("\n");
	if (!is_err(p))
		put(mem_eq((const char *)p, "HOLDER", 6) ?
		    "mapping matches /holder's data\n" :
		    "mapping does NOT match /holder's data\n");
	sys(__NR_write, from_parent[1], "x", 1, 0, 0, 0);
	wait_child(pid);
}

void start_main(long *sp)
{
	long argc = sp[0];
	char **argv = (char **)(sp + 1);
	const char *test = argc > 1 ? argv[1] : "";

	sys(__NR_mount, "proc", "/proc", "proc", 0, 0, 0);
	put("TEST-START ");
	put(test);
	put("\n");
	if (str_eq(test, "bigmap"))
		test_bigmap();
	else if (str_eq(test, "traced-map"))
		test_traced_map();
	else if (str_eq(test, "bigenv"))
		test_env(4, 28000);
	else if (str_eq(test, "smallenv"))
		test_env(1, 12000);
	else if (str_eq(test, "sweep"))
		test_sweep();
	else if (str_eq(test, "stack")) {
		test_env(1, 12000);
		test_sweep();
		test_env(4, 28000);
	}
	else if (str_eq(test, "devmem"))
		test_devmem();
	else if (str_eq(test, "devmem-alias"))
		test_devmem_alias();
	else
		put("unknown test\n");
	put("TEST-DONE\n");
	/*
	 * LINUX_REBOOT_CMD_POWER_OFF, then LINUX_REBOOT_CMD_RESTART, which
	 * QEMU's -no-reboot turns into an exit. The ColdFire power-off path
	 * spins, so m68k restarts directly.
	 */
#ifndef __m68k__
	sys(__NR_reboot, 0xfee1dead, 672274793, 0x4321fedc, 0, 0, 0);
#endif
	sys(__NR_reboot, 0xfee1dead, 672274793, 0x01234567, 0, 0, 0);
	for (;;)
		;
}
