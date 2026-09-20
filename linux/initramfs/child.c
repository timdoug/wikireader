// SPDX-License-Identifier: GPL-2.0
typedef unsigned int size_t;

long c33_write(int fd, const void *buffer, size_t count);

int child_main(void)
{
	static const char message[] = "C33 child: execve reached /child\n";

	c33_write(1, message, sizeof(message) - 1);
	return 23;
}
