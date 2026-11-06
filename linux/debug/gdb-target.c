/* A program for gdb-test.py to debug: arguments and locals, a global, a
 * recursion to unwind through, a loop to step, a call into libc, a signal
 * handler to unwind out of, a second thread, and sigsetjmp, the first call
 * to which goes through ld.so's lazy binding.  gdb-test.py finds the lines
 * it names by the markers in comments. */
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

int counter;
static sigjmp_buf jump;

static int depth(int n)
{
	if (n == 0)			/* @depth */
		return counter;
	counter += n;
	return depth(n - 1) + 1;	/* @depth-recurse */
}

int add(int a, int b)
{
	int sum = a + b;		/* @add */
	counter++;			/* @add-next */
	return sum;
}

static void on_signal(int sig)
{
	counter += sig;			/* @on-signal */
}

static void *worker(void *arg)
{
	counter += *(int *)arg;		/* @worker */
	return NULL;
}

int main(int argc, char **argv)
{
	const char *word = "wikireader";
	int total = 0;
	int step = 100;
	pthread_t thread;

	for (int i = 0; i < 3; i++)
		total = add(total, i);
	total += (int)strlen(word);	/* @strlen */
	total += depth(4);		/* @depth-call */
	signal(SIGUSR1, on_signal);
	raise(SIGUSR1);			/* @raise */
	pthread_create(&thread, NULL, worker, &step);
	pthread_join(thread, NULL);	/* @join */
	if (!sigsetjmp(jump, 1))	/* @sigsetjmp */
		siglongjmp(jump, 1);
	printf("GDB TARGET %d %d\n", total, counter);
	return 0;
}
