/* Common hooks for NEC C33 series.
   Copyright (C) 1996-2026 Free Software Foundation, Inc.

   This file is part of GCC.

   GCC is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3, or (at your option)
   any later version.

   GCC is distributed in the hope that it will be useful, but WITHOUT
   ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
   FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
   for more details.

   You should have received a copy of the GNU General Public License
   along with GCC; see the file COPYING3.  If not see
   <http://www.gnu.org/licenses/>.  */

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "diagnostic-core.h"
#include "tm.h"
#include "common/common-target.h"
#include "common/common-target-def.h"
#include "opts.h"
#include "flags.h"

/* Information about the various small memory areas.  */
static const int small_memory_physical_max[(int) SMALL_MEMORY_max] =
{
  256,
  65536,
  32768,
};

/* Set the maximum size of small memory area TYPE to the value given
   by SIZE in structure OPTS (option text OPT passed at location LOC).  */

static void
c33_handle_memory_option (enum small_memory_type type,
			   struct gcc_options *opts, const char *opt,
			   int size, location_t loc)
{
  if (size > small_memory_physical_max[type])
    error_at (loc, "value passed in %qs is too large", opt);
  else
    opts->x_small_memory_max[type] = size;
}

/* Implement TARGET_HANDLE_OPTION.  */

static bool
c33_handle_option (struct gcc_options *opts,
		    struct gcc_options *opts_set ATTRIBUTE_UNUSED,
		    const struct cl_decoded_option *decoded,
		    location_t loc)
{
  size_t code = decoded->opt_index;
  int value = decoded->value;

  switch (code)
    {
    /* Core selection is an Enum now (-mcore=, with -mc33/-mc33adv/-mc33pe
       as aliases), so the option machinery handles it and there is nothing
       to do here.  V850 needed this switch because its cores were a set of
       mutually exclusive masks.  */

    /* The C33 has one data area, addressed through %r15, which msda=
       sizes.  tda and zda are V850's other two; accept and ignore them so
       existing makefiles keep working.  */
    case OPT_mtda_:
    case OPT_mzda_:
      return true;

    case OPT_msda_:
      c33_handle_memory_option (SMALL_MEMORY_SDA, opts,
				 decoded->orig_option_with_args_text,
				 value, loc);
      return true;

    default:
      return true;
    }
}

/* Implement TARGET_OPTION_OPTIMIZATION_TABLE.  */

static const struct default_options c33_option_optimization_table[] =
  {
    /* -mep and -mprolog-function were V850's; neither option exists here.  */
    { OPT_LEVELS_NONE, 0, NULL, 0 }
  };

/* Nothing is on by default.  V850 turned on its core mask, MASK_APP_REGS
   and MASK_BIG_SWITCH here; the first two are gone, and switch tables
   default to 2-byte entries with -mbig-switch widening them.  */
#undef  TARGET_DEFAULT_TARGET_FLAGS
#define TARGET_DEFAULT_TARGET_FLAGS 0
#undef  TARGET_HANDLE_OPTION
#define TARGET_HANDLE_OPTION c33_handle_option
#undef  TARGET_OPTION_OPTIMIZATION_TABLE
#define TARGET_OPTION_OPTIMIZATION_TABLE c33_option_optimization_table

struct gcc_targetm_common targetm_common = TARGETM_COMMON_INITIALIZER;
