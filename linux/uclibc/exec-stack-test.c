// SPDX-License-Identifier: GPL-2.0-only
/* Exec an ELF with a 32 KiB runtime stack and a larger initial stack image.
 * Cover long argument strings, many argument pointers, and environment.
 * No-MMU copy_to_user cannot catch an undersized allocation. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define LENGTH 120000
#define WORDS 8000

static __attribute__((noinline)) int check_stack(void)
{
	volatile unsigned char stack[20000];
	for (unsigned i = 0; i < sizeof(stack); i++)
		stack[i] = (unsigned char)i;
	for (unsigned i = 0; i < sizeof(stack); i++)
		if (stack[i] != (unsigned char)i)
			return 1;
	return 0;
}

static int child(int argc, char **argv)
{
	if (check_stack())
		return 1;
	if (!strcmp(argv[1], "words")) {
		if (argc != WORDS + 2)
			return 1;
		for (int i = 2; i < argc; i++)
			if (strcmp(argv[i], "word"))
				return 1;
		return 0;
	}
	const char *text = !strcmp(argv[1], "env") ? getenv("PAYLOAD") : argv[2];
	if (!text || strlen(text) != LENGTH)
		return 1;
	for (unsigned i = 0; i < LENGTH; i++)
		if (text[i] != 'A' + i % 23)
			return 1;
	return 0;
}

static int run(char **argv, char **envp)
{
	pid_t pid = vfork();
	if (pid == 0) {
		execve(argv[0], argv, envp);
		_exit(125);
	}
	int status;
	return pid < 0 || waitpid(pid, &status, 0) != pid ||
		!WIFEXITED(status) || WEXITSTATUS(status);
}

int main(int argc, char **argv)
{
	if (argc > 1)
		return child(argc, argv);
	char *text = malloc(LENGTH + 9);
	char **words = calloc(WORDS + 3, sizeof(*words));
	if (!text || !words)
		return 1;
	memcpy(text, "PAYLOAD=", 8);
	for (unsigned i = 0; i < LENGTH; i++)
		text[i + 8] = 'A' + i % 23;
	text[LENGTH + 8] = 0;
	char *empty[] = { NULL };
	char *strings[] = { argv[0], "strings", text + 8, NULL };
	char *environment[] = { text, NULL };
	char *env_args[] = { argv[0], "env", NULL };
	words[0] = argv[0];
	words[1] = "words";
	for (int i = 2; i < WORDS + 2; i++)
		words[i] = "word";
	int failed = run(strings, empty) || run(words, empty) || run(env_args, environment);
	free(words);
	free(text);
	if (failed)
		return 1;
	puts("EXEC STACK PASS");
	return 0;
}
