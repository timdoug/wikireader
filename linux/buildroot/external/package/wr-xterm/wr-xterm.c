// SPDX-License-Identifier: GPL-2.0-only
/* xterm's supported -S interface avoids fork on a no-MMU Linux system.
 * A separately exec'd helper owns the slave, consumes xterm's window-ID
 * handshake, and execs the command. No child edits its parent's memory. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
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
static volatile sig_atomic_t stopped;

static void stop(int signal_number)
{
    stopped = signal_number;
}

static void fail(const char *what)
{
    perror(what);
    exit(1);
}

/* The on-screen keyboard, aimed at this terminal, which startx's session asks
 * for with WR_XTERM_KEYBOARD: started here, where the window is known, so the
 * shell runs nothing for it, and in a process group of its own, so that a ^C
 * typed in the terminal is not its. */
static void start_keyboard(const char *window, int slave)
{
    char *args[] = {"xkeyboard", (char *)window, NULL};
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    pid_t pid;

    if (posix_spawn_file_actions_init(&actions) != 0)
        return;
    if (posix_spawnattr_init(&attributes) != 0) {
        posix_spawn_file_actions_destroy(&actions);
        return;
    }
    for (int fd = 0; fd < 3; ++fd)
        posix_spawn_file_actions_addopen(&actions, fd, "/dev/null", O_RDWR, 0);
    if (slave > 2)
        posix_spawn_file_actions_addclose(&actions, slave);
    posix_spawnattr_setpgroup(&attributes, 0);
    /* uClibc forks for a new process group unless told vfork is safe, and
     * without an MMU it cannot fork. */
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_USEVFORK);
    if (posix_spawnp(&pid, "xkeyboard", &actions, &attributes, args, environ) != 0)
        fputs("xterm: cannot start xkeyboard\n", stderr);
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
}

static void child_command(const char *path, char **command)
{
    int slave;
    char window[64], ch;
    size_t used = 0;
    struct termios tty;

    if (setsid() < 0 || (slave = open(path, O_RDWR)) < 0 ||
        ioctl(slave, TIOCSCTTY, 0) < 0)
        fail("xterm: controlling terminal");
    if (tcgetattr(slave, &tty) < 0)
        fail("xterm: terminal attributes");
    /* -S writes the window ID to the slave before it starts terminal I/O.
     * ECHO is disabled by the parent so this protocol is never displayed. */
    for (;;) {
        ssize_t n = read(slave, &ch, 1);
        if (n < 0 && errno == EINTR)
            continue;
        if (n != 1 || used + 1 >= sizeof(window))
            fail("xterm: window handshake");
        if (ch == '\n')
            break;
        window[used++] = ch;
    }
    window[used] = '\0';
    char *end;
    unsigned long id = strtoul(window, &end, 16);
    if (!used || *end) {
        errno = EPROTO;
        fail("xterm: invalid window ID");
    }
    snprintf(window, sizeof(window), "%lu", id);
    if (setenv("WINDOWID", window, 1) < 0 || setenv("TERM", "xterm", 1) < 0)
        fail("xterm: environment");
    const char *keyboard = getenv("WR_XTERM_KEYBOARD");
    int want_keyboard = keyboard && *keyboard;
    /* A terminal started from this one should not start another keyboard. */
    unsetenv("WR_XTERM_KEYBOARD");
    if (want_keyboard)
        start_keyboard(window, slave);
    tty.c_lflag |= ECHO;
    if (tcsetattr(slave, TCSANOW, &tty) < 0 ||
        tcsetpgrp(slave, getpgrp()) < 0 ||
        dup2(slave, 0) < 0 || dup2(slave, 1) < 0 || dup2(slave, 2) < 0)
        fail("xterm: terminal setup");
    if (slave > 2)
        close(slave);
    execvp(command[0], command);
    perror(command[0]);
    exit(127);
}

static int spawn(pid_t *pid, const char *program, char **args,
                 posix_spawn_file_actions_t *actions)
{
    int error = posix_spawn(pid, program, actions, NULL, args, environ);
    if (error) {
        errno = error;
        perror(program);
    }
    return error;
}

static int input(int master, const char *bytes, size_t length)
{
    while (length) {
        ssize_t n = write(master, bytes, length);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        bytes += n;
        length -= (size_t)n;
    }
    return 0;
}

static int initial_command(int master, int slave, const char *command)
{
    /* The helper's handshake is canonical with echo off. Hush switches to
     * noncanonical, no-echo input when its interactive line editor is ready.
     * Wait for that transition rather than guessing a startup delay, for
     * up to 30 s: a server still starting slows the shell as well. */
    for (int n = 0; n < 3000 && !stopped; ++n) {
        struct termios tty;
        if (tcgetattr(slave, &tty) < 0)
            return -1;
        if (!(tty.c_lflag & (ICANON | ECHO))) {
            /* Ctrl-L clears the banner and redraws the shell's own prompt. */
            return input(master, "\f", 1) ||
                input(master, command, strlen(command)) || input(master, "\n", 1)
                ? -1 : 0;
        }
        struct timespec pause = {.tv_nsec = 10000000};
        nanosleep(&pause, NULL);
    }
    errno = stopped ? EINTR : ETIMEDOUT;
    return -1;
}

int main(int argc, char **argv)
{
    int master, slave, split = argc, hold = 0, status = 0, result = 0;
    int startup_failed = 0;
    pid_t terminal, command, ended;
    char path[64], option[96];
    char *shell[] = {"/bin/sh", "-l", NULL};
    struct termios tty;
    posix_spawn_file_actions_t actions;
    struct sigaction action = {.sa_handler = stop};

    if (argc >= 4 && strcmp(argv[1], "--pty-child") == 0) {
        child_command(argv[2], argv + 3);
        return 127;
    }
    const char *value = getenv("WR_XTERM_INITIAL_COMMAND");
    char *initial = value && *value ? strdup(value) : NULL;
    if (value && *value && !initial)
        fail("xterm: initial command");
    /* A nested terminal should not repeat this terminal's startup command. */
    unsetenv("WR_XTERM_INITIAL_COMMAND");
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-e") == 0) {
            split = i;
            if (i + 1 == argc) {
                fputs("xterm: -e requires a command\n", stderr);
                return 1;
            }
            break;
        }
        if (strcmp(argv[i], "-hold") == 0)
            hold = 1;
        if (strncmp(argv[i], "-S", 2) == 0 ||
            strcmp(argv[i], "-version") == 0 || strcmp(argv[i], "-help") == 0) {
            execv("/usr/libexec/xterm", argv);
            fail("xterm");
        }
    }
    master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || grantpt(master) < 0 || unlockpt(master) < 0 ||
        ptsname_r(master, path, sizeof(path)) != 0)
        fail("xterm: allocate PTY");
    slave = open(path, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (slave < 0 || tcgetattr(slave, &tty) < 0)
        fail("xterm: open slave");
    tty.c_lflag &= ~ECHO;
    if (tcsetattr(slave, TCSANOW, &tty) < 0)
        fail("xterm: disable handshake echo");

    char **client = calloc((size_t)split + 2, sizeof(*client));
    char **run = calloc((size_t)argc + 6, sizeof(*run));
    if (!client || !run)
        fail("xterm: arguments");
    client[0] = "/usr/libexec/xterm";
    snprintf(option, sizeof(option), "-S%s/%d", path, master);
    client[1] = option;
    for (int i = 1; i < split; ++i)
        client[i + 1] = argv[i];
    run[0] = "/usr/bin/xterm";
    run[1] = "--pty-child";
    run[2] = path;
    char **cmd = split == argc ? shell : argv + split + 1;
    for (int i = 0; cmd[i]; ++i)
        run[i + 3] = cmd[i];

    sigemptyset(&action.sa_mask);
    sigaction(SIGHUP, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addclose(&actions, master);
    int error = spawn(&command, run[0], run, &actions);
    posix_spawn_file_actions_destroy(&actions);
    if (error)
        return 1;
    if (spawn(&terminal, client[0], client, NULL)) {
        kill(command, SIGTERM);
        waitpid(command, NULL, 0);
        return 1;
    }
    if (initial && initial_command(master, slave, initial) < 0) {
        perror("xterm: initial command");
        result = 1;
        startup_failed = 1;
    }
    close(master);
    /* Keep the slave open for -hold so xterm does not see EOF on command exit. */
    if (!hold)
        close(slave);
    while (!stopped && !startup_failed) {
        ended = waitpid(-1, &status, 0);
        if (ended < 0 && errno == EINTR)
            continue;
        if (ended < 0)
            break;
        if (ended == terminal) {
            if (!result)
                result = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
            break;
        }
        if (ended == command) {
            command = -1;
            result = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
            if (!hold)
                break;
        }
    }
    if (command > 0) {
        kill(-command, SIGHUP);
        kill(command, SIGTERM);
        while (waitpid(command, NULL, 0) < 0 && errno == EINTR) {}
    }
    kill(terminal, SIGTERM);
    while (waitpid(terminal, NULL, 0) < 0 && errno == EINTR) {}
    if (hold)
        close(slave);
    free(client);
    free(run);
    free(initial);
    return stopped ? 128 + stopped : result;
}
