#include <setjmp.h>
#include <jmpbuf-offsets.h>

#define _JMPBUF_UNWINDS(jmpbuf, address) \
	((void *)(address) < (void *)(jmpbuf[JB_SP]))
