/* Unsigned 32-bit division for the Seiko Epson C33 PE core.

Copyright (C) 2026 Tim Douglas

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free
Software Foundation; either version 3, or (at your option) any later
version.

GCC is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

Under Section 7 of GPL version 3, you are granted additional
permissions described in the GCC Runtime Library Exception, version
3.1, as published by the Free Software Foundation.

You should have received a copy of the GNU General Public License and
a copy of the GCC Runtime Library Exception along with this program;
see the files COPYING3 and COPYING.RUNTIME respectively.  If not, see
<http://www.gnu.org/licenses/>.  */

/* This replaces libgcc's generic udivmodsi4.c, which the PE core reaches for
   on every / and % because the manual removes all five divide-step
   instructions.  The generic routine aligns the divisor a bit at a time and
   then produces all 32 quotient bits whatever the operands are: two loops,
   a taken branch per bit, about 1,600 cycles a divide on this machine.

   This one finds the same alignment in five tests instead of up to 32
   iterations, and the shift count it lands on is exactly the number of
   quotient bits there are to produce, so the second loop runs only those.
   Dividing a small number by a large one now costs the five tests and
   nothing else.

   Kept as a rolled loop deliberately.  Unrolling it is worth real time only
   in internal RAM (see riscv/rv32_div.s in the WikiReader tree); out in
   SDRAM, where a libgcc routine lands, the step body stays inside the
   core's 27-byte loop fetch window and is refetched from nothing, while an
   unrolled line would pay about three code words a step.

   The answers match the generic routine everywhere, including the two cases
   C leaves undefined: a zero divisor yields a zero quotient and the
   dividend as remainder.  */

unsigned long
__udivmodsi4 (unsigned long num, unsigned long den, int modwanted)
{
  unsigned long bit = 1;
  unsigned long quotient;

  if (!den || num < den)
    return modwanted ? num : 0;

  /* The largest k with (den << k) <= num, found greedily.  Each test is
     written as a shift of num rather than of den so that it cannot
     overflow: num >> s >= den holds exactly when num >= den << s.  */
  if ((num >> 16) >= den) { den <<= 16; bit <<= 16; }
  if ((num >> 8) >= den) { den <<= 8; bit <<= 8; }
  if ((num >> 4) >= den) { den <<= 4; bit <<= 4; }
  if ((num >> 2) >= den) { den <<= 2; bit <<= 2; }
  if ((num >> 1) >= den) { den <<= 1; bit <<= 1; }

  /* That top bit is always a one, so take it without a comparison.  */
  quotient = bit;
  num -= den;

  while ((bit >>= 1))
    {
      den >>= 1;
      if (num >= den)
	{
	  num -= den;
	  quotient |= bit;
	}
    }

  return modwanted ? num : quotient;
}
