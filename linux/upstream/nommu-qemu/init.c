/*
 * /init for the no-MMU reproducers. The kernel command line names one test
 * after "--"; init runs it, prints TEST-DONE and powers off.
 *
 *   bigmap        8 MiB private anonymous mmap (fixes/0045)
 *   traced-map    read-only private mmap of a ramfs file under ptrace (0042)
 *   bigenv        exec with a 112 KiB environment into an 8 KiB stack (0031)
 *   smallenv      exec with a 12 KB environment into an 8 KiB stack (0031)
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
	long r = sys(SYS_wait4, pid, &status, 0, 0, 0, 0);

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

	r = sys(SYS_ptrace, PTRACE_TRACEME, 0, 0, 0, 0, 0);
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
		sys(SYS_munmap, p, 4096, 0, 0, 0, 0);
	}
	return 0;
}

static void test_traced_map(void)
{
	wait_child(spawn(traced_map_child, CHILD_STACK));
}

/* fixes/0031: up to four 28000-byte environment strings, 112 KiB in all */
#define ENV_STRINGS 4
#define ENV_LEN 28000
static char env_buf[ENV_STRINGS][ENV_LEN];
static int env_strings, env_len;

static int env_child(void)
{
	char *argv[2];
	char *envp[ENV_STRINGS + 1];
	long r;
	int i, j;

	argv[0] = (char *)"/stack-victim";
	argv[1] = 0;
	for (i = 0; i < env_strings; i++) {
		env_buf[i][0] = 'A' + i;
		env_buf[i][1] = '=';
		for (j = 2; j < env_len - 1; j++)
			env_buf[i][j] = 'x';
		env_buf[i][env_len - 1] = 0;
		envp[i] = env_buf[i];
	}
	envp[env_strings] = 0;
	r = sys(SYS_execve, argv[0], argv, envp, 0, 0, 0);
	put("execve: ");
	put_result(r);
	put("\n");
	return 1;
}

static void test_env(int strings, int len)
{
	env_strings = strings;
	env_len = len;
	put("execve(/stack-victim) with ");
	put_dec(strings);
	put(" environment strings of ");
	put_dec(len);
	put(" bytes\n");
	wait_child(spawn(env_child, CHILD_STACK));
}

/* fixes/0032: the kernel's Image header contains "RISCV" at offset 0x30. */
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

	sys(SYS_dup3, to_parent[1], 4, 0, 0, 0, 0);
	sys(SYS_dup3, from_parent[0], 3, 0, 0, 0, 0);
	argv[0] = (char *)"/holder";
	argv[1] = 0;
	envp[0] = 0;
	sys(SYS_execve, argv[0], argv, envp, 0, 0, 0);
	return 1;
}

static void test_devmem_alias(void)
{
	int fds[2];
	unsigned long addr = 0;
	long pid, fd, p, r;

	sys(SYS_pipe2, fds, 0, 0, 0, 0, 0);
	to_parent[0] = fds[0];
	to_parent[1] = fds[1];
	sys(SYS_pipe2, fds, 0, 0, 0, 0, 0);
	from_parent[0] = fds[0];
	from_parent[1] = fds[1];
	pid = spawn(holder_child, CHILD_STACK);
	r = sys(SYS_read, to_parent[0], &addr, sizeof(addr), 0, 0, 0);
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
	sys(SYS_write, from_parent[1], "x", 1, 0, 0, 0);
	wait_child(pid);
}

void start_main(long *sp)
{
	long argc = sp[0];
	char **argv = (char **)(sp + 1);
	const char *test = argc > 1 ? argv[1] : "";

	sys(SYS_mount, "proc", "/proc", "proc", 0, 0, 0);
	put("TEST-START ");
	put(test);
	put("\n");
	if (str_eq(test, "bigmap"))
		test_bigmap();
	else if (str_eq(test, "traced-map"))
		test_traced_map();
	else if (str_eq(test, "bigenv"))
		test_env(ENV_STRINGS, ENV_LEN);
	else if (str_eq(test, "smallenv"))
		test_env(1, 12000);
	else if (str_eq(test, "devmem"))
		test_devmem();
	else if (str_eq(test, "devmem-alias"))
		test_devmem_alias();
	else
		put("unknown test\n");
	put("TEST-DONE\n");
	/* LINUX_REBOOT_CMD_POWER_OFF; QEMU's -no-reboot covers a restart. */
	sys(SYS_reboot, 0xfee1dead, 672274793, 0x4321fedc, 0, 0, 0);
	sys(SYS_reboot, 0xfee1dead, 672274793, 0x01234567, 0, 0, 0);
	for (;;)
		;
}
