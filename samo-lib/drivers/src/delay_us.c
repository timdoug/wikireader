/*
 * busy waiting delay routine
 *
 * Copyright (c) 2009 Openmoko Inc.
 *
 * Authors   Christopher Hall <hsw@openmoko.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "delay.h"

void delay_us(unsigned int microsec)
{
	while (microsec--) {
		// at 60 MHz this should take 1 micro second
		//
		// r4 and the flags are clobbered, and the asm says so: with no
		// clobber list the compiler was free to keep a live value in
		// r4 across the loop.  The label is a local "0:" because a
		// named label is emitted once per expansion and fails to
		// assemble as soon as the loop is unrolled or inlined twice.
		asm volatile (
			"\tld.w\t%%r4, 7\n"
			"0:\n"
			"\tnop\n"
			"\tnop\n"
			"\tsub\t%%r4, 1\n"
			"\tjrne\t0b"
			:
			:
			: "r4", "cc");
	}
}
