// SPDX-License-Identifier: GPL-2.0-only
/* Executed inside xterm, after the launcher has consumed the -S handshake. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <termios.h>
#include <unistd.h>

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
    if (uname(&name) != 0)
        return 1;
    printf("%s %s %s %s %s\n", name.sysname, name.nodename,
           name.release, name.version, name.machine);
    FILE *report = fopen("/mnt/sd/xterm.txt", "w");
    if (!report)
        return 1;
    fprintf(report, "tty=%d session=%d geometry=%d environment=%d\n",
            tty, session, geometry, environment);
    int passed = tty && session && geometry && environment;
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
