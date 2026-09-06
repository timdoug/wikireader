/*
 * sdram - reprogram the SDRAM controller's timing after boot
 *
 * Copyright (c) 2026 Tim Douglas
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

#if !defined(_SDRAM_H_)
#define _SDRAM_H_ 1

#include "standard.h"

/* Call once, after Suspend_initialise has copied the A0 RAM code. */
void SDRAM_retime(void);

#endif
