#include <setjmp.h>
#include <jmpbuf-offsets.h>

#define _JMPBUF_UNWINDS(jmpbuf, address) \
	((void *)(address) < (void *)(jmpbuf[JB_SP]))

#ifdef __UCLIBC_HAS_THREADS_NATIVE__
#include <stdint.h>
#include <unwind.h>
#define _JMPBUF_UNWINDS_ADJ(buf, address, adj) \
	((uintptr_t)(address) - (adj) < (uintptr_t)(buf)[JB_SP] - (adj))
#define _JMPBUF_CFA_UNWINDS_ADJ(buf, context, adj) \
	_JMPBUF_UNWINDS_ADJ(buf, (void *)_Unwind_GetCFA(context), adj)
#endif
