// SPDX-License-Identifier: GPL-2.0
typedef unsigned int size_t;

long c33_fcntl(int fd, int command, unsigned long argument);
long c33_read(int fd, void *buffer, size_t count);
long c33_write(int fd, const void *buffer, size_t count);
long c33_getpid(void);
long c33_clone(unsigned long flags, void *stack, void *parent_tid,
	       void *child_tid, unsigned long tls);
long c33_execve(const char *path, char *const argv[], char *const envp[]);
long c33_wait4(long pid, int *status, int options, void *rusage);
void c33_exit(int status);
long c33_kill(long pid, int signal);
long c33_rt_sigaction(int signal, const void *action, void *old_action,
		      size_t signal_set_size);

enum message {
	MESSAGE_BANNER,
	MESSAGE_HELP,
	MESSAGE_PROMPT,
	MESSAGE_RX,
	MESSAGE_PID,
	MESSAGE_COMMANDS,
	MESSAGE_PROCESS_PASS,
	MESSAGE_PROCESS_FAIL,
	MESSAGE_SIGNAL_PASS,
	MESSAGE_SIGNAL_FAIL,
	MESSAGE_LIBC_PASS,
	MESSAGE_LIBC_FAIL,
	MESSAGE_BUSYBOX_PASS,
	MESSAGE_BUSYBOX_FAIL,
	MESSAGE_NEWLINE,
};

struct message_record {
	const char *text;
	size_t size;
};

#define MESSAGE(text) { text, sizeof(text) - 1 }

static const struct message_record messages[] = {
	MESSAGE("*** HARDWARE PASS: native C33 Linux reached PID 1 ***\n"),
	MESSAGE("c33 shell: h=help p=getpid c=command-count\n"),
	MESSAGE("c33> "),
	MESSAGE("UART RX reached Linux userspace.\n"),
	MESSAGE("pid "),
	MESSAGE("commands "),
	MESSAGE("C33 process test: clone -> execve -> wait4 passed\n"),
	MESSAGE("C33 process test FAILED\n"),
	MESSAGE("C33 signal test: handler -> rt_sigreturn passed\n"),
	MESSAGE("C33 signal test FAILED\n"),
	MESSAGE("C33 libc test: crt -> stdio -> getpid -> longjmp passed\n"),
	MESSAGE("C33 libc test FAILED\n"),
	MESSAGE("C33 BusyBox test: hush -> echo -> exit passed\n"),
	MESSAGE("C33 BusyBox test FAILED\n"),
	MESSAGE("\n"),
};

static unsigned int command_count;
static volatile unsigned char digit_base = '0';
static volatile unsigned int signal_seen;
static const char *child_path;
static char *const *child_argv;

struct c33_sigaction {
	void (*handler)(int);
	unsigned long flags;
	unsigned long mask[2];
};

static void write_all(const char *buffer, size_t count)
{
	while (count) {
		long written = c33_write(1, buffer, count);

		if (written <= 0)
			continue;
		buffer += written;
		count -= written;
	}
}

static void put_message(enum message index)
{
	write_all(messages[index].text, messages[index].size);
}

void clone_child(void)
{
	char *envp[] = { 0 };

	c33_execve(child_path, child_argv, envp);
	c33_exit(127);
}

static int run_program(const char *path, char *const argv[], int expected_status)
{
	long pid;
	long waited;
	int status = 0;

	child_path = path;
	child_argv = argv;
	/* The asm-generic no-MMU vfork ABI is clone(CLONE_VM|CLONE_VFORK). */
	pid = c33_clone(0x4111, 0, 0, 0, 0);
	if (pid <= 0)
		return 0;
	waited = c33_wait4(pid, &status, 0, 0);
	return waited == pid && status == (expected_status << 8);
}

static void process_test(void)
{
	char *argv[] = { "/child", 0 };

	if (run_program("/child", argv, 23))
		put_message(MESSAGE_PROCESS_PASS);
	else
		put_message(MESSAGE_PROCESS_FAIL);
}

static void libc_test(void)
{
	char *argv[] = { "/uclibc-smoke", 0 };

	if (run_program("/uclibc-smoke", argv, 0))
		put_message(MESSAGE_LIBC_PASS);
	else
		put_message(MESSAGE_LIBC_FAIL);
}

static void busybox_test(void)
{
	char *argv[] = {
		"sh", "-c",
		"echo C33 BusyBox 1.38.0: hush and echo reached userspace",
		0,
	};

	if (run_program("/busybox", argv, 0))
		put_message(MESSAGE_BUSYBOX_PASS);
	else
		put_message(MESSAGE_BUSYBOX_FAIL);
}

static void signal_handler(int signal)
{
	signal_seen = signal;
}

static void signal_test(void)
{
	struct c33_sigaction action = {
		.handler = signal_handler,
		.flags = 0,
		.mask = { 0, 0 },
	};

	signal_seen = 0;
	if (c33_rt_sigaction(10, &action, 0, sizeof(action.mask)) == 0 &&
	    c33_kill(c33_getpid(), 10) == 0 && signal_seen == 10)
		put_message(MESSAGE_SIGNAL_PASS);
	else
		put_message(MESSAGE_SIGNAL_FAIL);
}

void init_main(void)
{
	char input;
	char number;
	int rx_reported = 0;

	c33_fcntl(0, 4, 0x800);
	process_test();
	signal_test();
	libc_test();
	busybox_test();
	put_message(MESSAGE_BANNER);
	put_message(MESSAGE_HELP);
	put_message(MESSAGE_PROMPT);

	for (;;) {
		if (c33_read(0, &input, 1) != 1)
			continue;

		if (!rx_reported) {
			put_message(MESSAGE_RX);
			rx_reported = 1;
		}
		command_count++;
		if (input == 'h') {
			put_message(MESSAGE_HELP);
		} else if (input == 'p') {
			put_message(MESSAGE_PID);
			number = digit_base + c33_getpid();
			write_all(&number, 1);
			put_message(MESSAGE_NEWLINE);
		} else if (input == 'c') {
			put_message(MESSAGE_COMMANDS);
			number = digit_base + command_count;
			write_all(&number, 1);
			put_message(MESSAGE_NEWLINE);
		}
		put_message(MESSAGE_PROMPT);
	}
}
