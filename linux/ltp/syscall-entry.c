/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Generic syscall entry must honor ptrace's number and return-value edits. */
#include "io-checks.h"
#include <elf.h>
#include <signal.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>
#include <asm/ptrace.h>

#define UNKNOWN_CALL 0x7fff01
#define REWRITE_CALL 0x7fff02
#define SKIP_CALL 0x7fff03
#define INJECTED_RESULT 12345

static int child(pid_t parent)
{
    errno = 0;
    CHECK(syscall(UNKNOWN_CALL) == -1 && errno == ENOSYS);
    CHECK(syscall(REWRITE_CALL) == parent);
    CHECK(syscall(SKIP_CALL) == INJECTED_RESULT);
    return finish("unknown, rewritten and skipped syscalls");
}

int main(int argc, char **argv)
{
    pid_t pid;
    int status, stops = 0, rewrites = 0, skips = 0;
    char parent[32];
    char *const child_argv[] = {argv[0], "child", parent, NULL};

    if (argc == 3 && strcmp(argv[1], "child") == 0)
        return child((pid_t)strtol(argv[2], NULL, 10));

    snprintf(parent, sizeof(parent), "%ld", (long)getpid());
    pid = vfork();
    if (pid == 0) {
        if (ptrace(PTRACE_TRACEME, 0, 0, 0) != 0)
            _exit(126);
        execv(argv[0], child_argv);
        _exit(127);
    }
    REQUIRE(pid > 0);
    REQUIRE(waitpid(pid, &status, 0) == pid && WIFSTOPPED(status));
    REQUIRE(ptrace(PTRACE_SETOPTIONS, pid, 0, PTRACE_O_TRACESYSGOOD) == 0);
    for (;;) {
        REQUIRE(ptrace(PTRACE_SYSCALL, pid, 0, 0) == 0);
        REQUIRE(waitpid(pid, &status, 0) == pid);
        if (!WIFSTOPPED(status))
            break;
        REQUIRE(++stops < 512);
        REQUIRE(WSTOPSIG(status) == (SIGTRAP | 0x80));
        struct user_pt_regs regs;
        struct iovec iov = {&regs, sizeof(regs)};
        REQUIRE(ptrace(PTRACE_GETREGSET, pid, NT_PRSTATUS, &iov) == 0);
        REQUIRE(iov.iov_len == sizeof(regs));
        if (regs.orig_r4 == REWRITE_CALL || regs.orig_r4 == SKIP_CALL) {
            CHECK((long)regs.r[4] == -ENOSYS);
            if (regs.orig_r4 == REWRITE_CALL) {
                regs.orig_r4 = SYS_getppid;
                ++rewrites;
            } else {
                regs.orig_r4 = -1UL;
                regs.r[4] = INJECTED_RESULT;
                ++skips;
            }
            REQUIRE(ptrace(PTRACE_SETREGSET, pid, NT_PRSTATUS, &iov) == 0);
        }
    }
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(rewrites == 1 && skips == 1);
    return finish("ptrace syscall number rewrite and skipped return preservation");
}
