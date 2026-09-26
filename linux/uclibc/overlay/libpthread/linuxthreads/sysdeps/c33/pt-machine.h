/* Machine-dependent pthreads configuration and inline functions.
   Epson S1C33 version.

   The GNU C Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public License as
   published by the Free Software Foundation; either version 2.1 of the
   License, or (at your option) any later version.

   The GNU C Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with the GNU C Library; see the file COPYING.LIB.  If
   not, see <http://www.gnu.org/licenses/>.  */

#ifndef _PT_MACHINE_H
#define _PT_MACHINE_H   1

#include <features.h>

#ifndef PT_EI
# define PT_EI __extern_always_inline
#endif

/* Spinlock implementation; required.

   The C33 has no atomic read-modify-write, but it is a single core with no
   privilege levels, so user code may mask interrupts itself: with IE clear
   nothing can run between the load and the store.  The H8/300 port does the
   same.  Restoring the whole PSR puts IE back as it was.  */
PT_EI long int
testandset (int *spinlock)
{
  int ret, psr;

  __asm__ __volatile__ (
      "ld.w %1,%%psr\n\t"
      "psrclr 4\n\t"
      "ld.w %0,[%3]\n\t"
      "ld.w [%3],%2\n\t"
      "ld.w %%psr,%1"
      : "=&r" (ret), "=&r" (psr)
      : "r" (1), "r" (spinlock)
      : "memory", "cc");

  return ret;
}

/* Get some notion of the current stack.  Need not be exactly the top
   of the stack, just something somewhere in the current frame.  */
#define CURRENT_STACK_FRAME  ((char *) __builtin_frame_address (0))

#endif /* pt-machine.h */
