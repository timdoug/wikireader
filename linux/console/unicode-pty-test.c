// SPDX-License-Identifier: GPL-2.0-only
/* Exercise the installed Hush line editor through a real Unix98 PTY. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static int expect_output(int master, const char *expected)
{
	char output[16384];
	size_t used = 0;
	struct pollfd descriptor = { .fd = master, .events = POLLIN };
	struct timespec start, now;

	clock_gettime(CLOCK_MONOTONIC, &start);
	for (;;) {
		clock_gettime(CLOCK_MONOTONIC, &now);
		if (now.tv_sec - start.tv_sec >= 10 || used == sizeof(output) - 1)
			break;
		if (poll(&descriptor, 1, 100) <= 0)
			continue;
		ssize_t n = read(master, output + used, sizeof(output) - used - 1);
		if (n <= 0)
			break;
		used += n;
		output[used] = '\0';
		const char *found = strstr(output, expected);
		if (found && strstr(found + strlen(expected), "U> "))
			return 1;
	}
	output[used] = '\0';
	fprintf(stderr, "Expected %s followed by a prompt, received:\n%s\n", expected, output);
	return 0;
}

static int command(int master, const char *input, const char *expected)
{
	size_t length = strlen(input);
	while (length) {
		ssize_t n = write(master, input, length);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return 0;
		input += n;
		length -= n;
	}
	return expect_output(master, expected);
}

int main(int argc, char **argv)
{
	if (argc == 3 && !strcmp(argv[1], "--shell")) {
		if (setsid() < 0)
			return 126;
		int slave = open(argv[2], O_RDWR);
		if (slave < 0 || ioctl(slave, TIOCSCTTY, 0) < 0 ||
		    dup2(slave, 0) < 0 || dup2(slave, 1) < 0 || dup2(slave, 2) < 0)
			return 126;
		if (slave > 2)
			close(slave);
		execl("/bin/sh", "sh", "-l", "-i", (char *)NULL);
		return 127;
	}
	char directory[] = "/tmp/wr-unicode-XXXXXX";
	int master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (master < 0 || grantpt(master) || unlockpt(master) || !mkdtemp(directory))
		return 1;
	char *name = ptsname(master);
	if (!name)
		return 1;
	int slave = open(name, O_RDWR | O_NOCTTY | O_CLOEXEC);
	struct termios tty;
	struct winsize size = { .ws_row = 15, .ws_col = 40 };
	if (slave < 0 || tcgetattr(slave, &tty))
		return 1;
	tty.c_iflag |= IUTF8;
	if (tcsetattr(slave, TCSANOW, &tty) || ioctl(slave, TIOCSWINSZ, &size))
		return 1;
	setenv("LANG", "C.UTF-8", 1);
	unsetenv("LC_ALL");
	unsetenv("LC_CTYPE");
	setenv("TERM", "linux", 1);
	setenv("WR_UNICODE_DIR", directory, 1);
	char *args[] = { argv[0], "--shell", name, NULL };
	pid_t child;
	if (posix_spawn(&child, argv[0], NULL, NULL, args, environ))
		return 1;
	/* Wait until Hush has installed its line editor, before sending input. */
	int ready = 0;
	for (unsigned int i = 0; i < 500; i++) {
		if (!tcgetattr(slave, &tty) && !(tty.c_lflag & (ICANON | ECHO))) {
			ready = 1;
			break;
		}
		poll(NULL, 0, 10);
	}
	int passed = ready && command(master,
		"PS1='U> '; printf '\\nU-READY\\n'\r", "\r\nU-READY\r\n");
	passed = passed && command(master,
		"v='caf\xc3\xa9\177Z'; printf '\\nU-LATIN:%s:%s\\n' \"$v\" \"${#v}\"\r",
		"\r\nU-LATIN:cafZ:4\r\n");
	passed = passed && command(master,
		"v='\xc3\xa9\xce\xbb\033[D\177\xd0\x96\033[C'; printf '\\nU-ARROW:%s:%s\\n' \"$v\" \"${#v}\"\r",
		"\r\nU-ARROW:\xd0\x96\xce\xbb:2\r\n");
	passed = passed && command(master,
		"v='A\xe6\x97\xa5\177B'; printf '\\nU-WIDE:%s:%s\\n' \"$v\" \"${#v}\"\r",
		"\r\nU-WIDE:AB:2\r\n");
	passed = passed && command(master,
		"v='e\xcc\x81'; printf '\\nU-COMB:%s:%s\\n' \"$v\" \"${#v}\"\r",
		"\r\nU-COMB:e\xcc\x81:2\r\n");
	passed = passed && command(master,
		"cd \"$WR_UNICODE_DIR\"; printf 'FILE-CONTENT' > caf\xc3\xa9; printf '\\nU-FILE\\n'\r",
		"\r\nU-FILE\r\n");
	passed = passed && command(master,
		"cat caf\t; printf '\\nU-TAB:%s\\n' \"$(ls)\"\r",
		"FILE-CONTENT\r\nU-TAB:caf\xc3\xa9\r\n");
	/* The file itself and wc must retain UTF-8 rather than substituted bytes. */
	passed = passed && command(master,
		"[ \"$(ls | wc -m)\" = 5 ] && rm caf\xc3\xa9 && cd / && rmdir \"$WR_UNICODE_DIR\" && printf '\\nU-CLEAN\\n'\r",
		"\r\nU-CLEAN\r\n");
	kill(-child, SIGKILL);
	waitpid(child, NULL, 0);
	close(slave);
	close(master);
	printf("UNICODE PTY %s: UTF-8 editing, arrows, wide characters, combining marks and filename completion\n",
	       passed ? "PASS" : "FAIL");
	return !passed;
}
