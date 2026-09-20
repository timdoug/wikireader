// SPDX-License-Identifier: GPL-2.0
typedef unsigned int size_t;

long c33_fcntl(int fd, int command, unsigned long argument);
long c33_read(int fd, void *buffer, size_t count);
long c33_write(int fd, const void *buffer, size_t count);
long c33_getpid(void);

enum message {
	MESSAGE_BANNER,
	MESSAGE_HELP,
	MESSAGE_PROMPT,
	MESSAGE_RX,
	MESSAGE_PID,
	MESSAGE_NEWLINE,
};

static const char *message_at(const char *table, unsigned int index)
{
	while (index--)
		table += 1 + (unsigned char)table[0];
	return table;
}

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

static void put_message(const char *table, enum message index)
{
	const char *message = message_at(table, index);

	write_all(message + 1, (unsigned char)message[0]);
}

void init_main(const char *messages)
{
	char input;
	char pid;
	int rx_reported = 0;

	c33_fcntl(0, 4, 0x800);
	put_message(messages, MESSAGE_BANNER);
	put_message(messages, MESSAGE_HELP);
	put_message(messages, MESSAGE_PROMPT);

	for (;;) {
		if (c33_read(0, &input, 1) != 1)
			continue;

		if (!rx_reported) {
			put_message(messages, MESSAGE_RX);
			rx_reported = 1;
		}
		if (input == 'h') {
			put_message(messages, MESSAGE_HELP);
		} else if (input == 'p') {
			put_message(messages, MESSAGE_PID);
			pid = '0' + c33_getpid();
			write_all(&pid, 1);
			put_message(messages, MESSAGE_NEWLINE);
		}
		put_message(messages, MESSAGE_PROMPT);
	}
}
