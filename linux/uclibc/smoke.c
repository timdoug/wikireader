#include <setjmp.h>
#include <stdio.h>
#include <unistd.h>

static jmp_buf jump_buffer;

int main(void)
{
	int jumped = setjmp(jump_buffer);

	if (jumped == 0)
		longjmp(jump_buffer, 7);
	printf("C33 uClibc smoke: pid=%ld longjmp=%d\n",
	       (long)getpid(), jumped);
	return 0;
}
