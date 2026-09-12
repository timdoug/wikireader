/****************************************************************************
 * apps/interpreters/micropython/modtime_port.c
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

/* The parts of the time module only the operating system can answer.  This
 * is not compiled on its own: MicroPython's extmod/modtime.c includes it,
 * which is how a port fills in the two holes that module leaves.
 *
 * The clock behind all of this is NuttX's, which on this board starts at the
 * epoch and is set by whoever set it -- so time.time() is as true as the
 * last person to say what time it was.
 */

#include <time.h>

#include "py/obj.h"
#include "shared/timeutils/timeutils.h"

static mp_obj_t mp_time_time_get(void)
{
  /* Seconds, as an integer: a single-precision float runs out of mantissa
   * for this number in 2004 and would count in steps of 128 seconds.
   */

  return mp_obj_new_int((mp_int_t)time(NULL));
}

static void mp_time_localtime_get(timeutils_struct_time_t *tm)
{
  timeutils_seconds_since_epoch_to_struct_time((mp_timestamp_t)time(NULL),
                                               tm);
}
