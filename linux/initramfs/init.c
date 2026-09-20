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

enum message {
	MESSAGE_BANNER,
	MESSAGE_HELP,
	MESSAGE_PROMPT,
	MESSAGE_RX,
	MESSAGE_PID,
	MESSAGE_COMMANDS,
	MESSAGE_PROCESS_PASS,
	MESSAGE_PROCESS_FAIL,
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
	MESSAGE("\n"),
};

static unsigned int command_count;
static volatile unsigned char digit_base = '0';
static unsigned char child_stack[4096] __attribute__((aligned(16)));

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
	static char child_path[] = "/child";
	char *argv[] = { child_path, 0 };
	char *envp[] = { 0 };

	c33_execve(child_path, argv, envp);
	c33_exit(127);
}

static void process_test(void)
{
	long pid;
	long waited;
	int status = 0;

	/* clone(CLONE_VM | SIGCHLD) with a private child stack. */
	pid = c33_clone(0x111, child_stack + sizeof(child_stack), 0, 0, 0);
	if (pid <= 0) {
		put_message(MESSAGE_PROCESS_FAIL);
		return;
	}
	waited = c33_wait4(pid, &status, 0, 0);
	if (waited == pid && status == (23 << 8))
		put_message(MESSAGE_PROCESS_PASS);
	else
		put_message(MESSAGE_PROCESS_FAIL);
}

void init_main(void)
{
	char input;
	char number;
	int rx_reported = 0;

	c33_fcntl(0, 4, 0x800);
	process_test();
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
