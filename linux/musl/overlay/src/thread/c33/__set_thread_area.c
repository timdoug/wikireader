#include "syscall.h"

int __set_thread_area(void *p)
{
	return __syscall(SYS_c33_set_tls, p);
}
