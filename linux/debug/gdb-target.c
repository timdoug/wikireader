/* A program for gdb-test.py to debug: arguments and locals, a global, a
 * recursion to unwind through, a loop to step, a call into libc, and a
 * signal handler to unwind out of. */
#include <signal.h>
#include <stdio.h>
#include <string.h>

int counter;

static int depth(int n)
{
	if (n == 0)
		return counter;
	counter += n;
	return depth(n - 1) + 1;
}

int add(int a, int b)
{
	int sum = a + b;
	counter++;
	return sum;
}

static void on_signal(int sig)
{
	counter += sig;
}

int main(int argc, char **argv)
{
	const char *word = "wikireader";
	int total = 0;

	for (int i = 0; i < 3; i++)
		total = add(total, i);
	total += (int)strlen(word);
	total += depth(4);
	signal(SIGUSR1, on_signal);
	raise(SIGUSR1);
	printf("GDB TARGET %d %d\n", total, counter);
	return 0;
}
