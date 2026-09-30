/* C33 Linux signal frame unwinding.
   Copyright (C) 2026 Free Software Foundation, Inc.

   This file is part of GCC, distributed under the GNU GPL version 3
   or later with the GCC Runtime Library Exception version 3.1.
   See COPYING3 and COPYING.RUNTIME. */

#ifndef inhibit_libc
#include <signal.h>
#include <stdint.h>
#include <sys/ucontext.h>

#define MD_FALLBACK_FRAME_STATE_FOR c33_fallback_frame_state

static _Unwind_Reason_Code
c33_fallback_frame_state (struct _Unwind_Context *context,
                          _Unwind_FrameState *fs)
{
  const uint16_t *pc = context->ra;
  /* Kernel trampoline: xld.w %r4,139; int 0. Read halfwords because
     C33 instructions need only two-byte alignment. */
  if (pc[0] != 0xc002 || pc[1] != 0x6cb4 || pc[2] != 0x0480)
    return _URC_END_OF_STACK;

  struct rt_sigframe
  {
    unsigned long return_address;
    siginfo_t info;
    ucontext_t uc;
  };
  /* The handler's CFA is just past the stack word holding its return PC. */
  struct rt_sigframe *frame = (void *)((char *)context->cfa - 4);
  mcontext_t *sc = &frame->uc.uc_mcontext;
  _Unwind_Ptr new_cfa = sc->sp;
  fs->regs.cfa_how = CFA_REG_OFFSET;
  fs->regs.cfa_reg = 16;
  fs->regs.cfa_offset = new_cfa - (_Unwind_Ptr)context->cfa;
  for (int i = 0; i < 16; i++)
    {
      fs->regs.how[i] = REG_SAVED_OFFSET;
      fs->regs.reg[i].loc.offset = (_Unwind_Ptr)&sc->r[i] - new_cfa;
    }
  fs->regs.how[16] = REG_SAVED_OFFSET;
  fs->regs.reg[16].loc.offset = (_Unwind_Ptr)&sc->sp - new_cfa;
  fs->retaddr_column = 22;
  fs->regs.how[22] = REG_SAVED_OFFSET;
  fs->regs.reg[22].loc.offset = (_Unwind_Ptr)&sc->pc - new_cfa;
  fs->signal_frame = 1;
  return _URC_NO_REASON;
}
#endif
