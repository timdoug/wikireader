#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

int main(int argc, char **argv)
{
	char *child_argv[] = {argv[0], "verify", "two", "", "four", NULL};
	int mode, status;
	pid_t pid;
	if (argc != 1) {
		int ok = argc >= 2 && argv[1] && !strcmp(argv[1], "verify") &&
			(argc == 2 || (argc == 5 && argv[2] && argv[3] && argv[4] &&
			 !strcmp(argv[2], "two") && !strcmp(argv[3], "") &&
			 !strcmp(argv[4], "four")));
		printf("EXEC child: argc=%d", argc);
		for (mode = 0; mode < argc; ++mode)
			printf(" argv[%d]=%s", mode, argv[mode] ? argv[mode] : "<null>");
		puts("");
		return ok ? 0 : 1;
	}
	for (mode = 0; mode < 8; ++mode) {
		child_argv[2] = mode < 4 ? "two" : NULL;
		pid = vfork();
		if (pid == 0) {
			switch (mode) {
			case 0: execv(argv[0], child_argv); break;
			case 1: execl(argv[0], argv[0], "verify", "two", "", "four", (char *)NULL); break;
			case 2: execle(argv[0], argv[0], "verify", "two", "", "four", (char *)NULL, environ); break;
			case 3: execlp(argv[0], argv[0], "verify", "two", "", "four", (char *)NULL); break;
			case 4: execv(argv[0], child_argv); break;
			case 5: execl(argv[0], argv[0], "verify", (char *)NULL); break;
			case 6: execle(argv[0], argv[0], "verify", (char *)NULL, environ); break;
			case 7: execlp(argv[0], argv[0], "verify", (char *)NULL); break;
			}
			_exit(127);
		}
		if (pid < 0 || waitpid(pid, &status, 0) != pid ||
		    !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
			printf("EXEC ARGUMENTS FAIL: mode=%d\n", mode);
			return 1;
		}
	}
	puts("EXEC ARGUMENTS PASS: vfork plus execv/execl/execle/execlp");
	return 0;
}
