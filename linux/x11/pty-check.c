// SPDX-License-Identifier: GPL-2.0-only
/* Executed inside xterm, after the launcher has consumed the -S handshake. */
#define _GNU_SOURCE
#include <locale.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <termios.h>
#include <unistd.h>
#include <wchar.h>

/* Ask the actual xterm where Unicode output left its cursor. */
static int unicode_cursor(void)
{
    struct termios saved, raw;
    if (tcgetattr(0, &saved) || !(saved.c_iflag & IUTF8))
        return 0;
    raw = saved;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(0, TCSANOW, &raw))
        return 0;
    printf("\033[2J\033[H\xc3\xa9\xce\xbb\xd0\x96\xe2\x82\xac\xe6\x97\xa5" "e\xcc\x81\033[6n");
    fflush(stdout);
    char reply[64];
    size_t used = 0;
    struct pollfd fd = { .fd = 0, .events = POLLIN };
    while (used + 1 < sizeof(reply) && poll(&fd, 1, 3000) > 0) {
        ssize_t count = read(0, reply + used, sizeof(reply) - used - 1);
        if (count <= 0)
            break;
        used += count;
        if (memchr(reply, 'R', used))
            break;
    }
    reply[used] = '\0';
    tcsetattr(0, TCSANOW, &saved);
    return strcmp(reply, "\033[1;8R") == 0;
}

int main(void)
{
    struct winsize size;
    struct utsname name;
    const char *window = getenv("WINDOWID"), *term = getenv("TERM");
    int tty = isatty(0) && isatty(1) && isatty(2);
    int session = getsid(0) == tcgetsid(0) && tcgetpgrp(0) == getpgrp();
    int geometry = ioctl(0, TIOCGWINSZ, &size) == 0 &&
        size.ws_col == 32 && size.ws_row == 22;
    int environment = term && strcmp(term, "xterm") == 0 &&
        window && strtoul(window, NULL, 10) != 0;
    int unicode = setlocale(LC_CTYPE, "") && MB_CUR_MAX > 1 &&
        wcwidth(0x65e5) == 2 && unicode_cursor();
    if (uname(&name) != 0)
        return 1;
    printf("%s %s %s %s %s\n", name.sysname, name.nodename,
           name.release, name.version, name.machine);
    FILE *report = fopen("/mnt/sd/xterm.txt", "w");
    if (!report)
        return 1;
    fprintf(report, "tty=%d session=%d geometry=%d environment=%d unicode=%d\n",
            tty, session, geometry, environment, unicode);
    int passed = tty && session && geometry && environment && unicode;
    fprintf(report, "XTERM PTY %s\n", passed ? "PASS" : "FAIL");
    if (fclose(report) != 0)
        return 1;
    FILE *shell = fopen("/mnt/sd/xshell.txt", "w");
    if (!shell)
        return 1;
    fprintf(shell, "%ld %s\n", (long)getppid(), ttyname(0));
    if (fclose(shell) != 0)
        return 1;
    fflush(stdout);
    return passed ? 0 : 1;
}
