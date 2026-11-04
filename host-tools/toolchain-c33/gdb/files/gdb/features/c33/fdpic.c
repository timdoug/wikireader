/* THIS FILE IS GENERATED.  -*- buffer-read-only: t -*- vi:set ro:
  Original: fdpic.xml */

#include "gdbsupport/tdesc.h"

static int
create_feature_c33_fdpic (struct target_desc *result, long regnum)
{
  struct tdesc_feature *feature;

  feature = tdesc_create_feature (result, "org.gnu.gdb.c33.fdpic");
  tdesc_create_reg (feature, "loadmap_exec", regnum++, 1, "system", 32, "data_ptr");
  tdesc_create_reg (feature, "loadmap_interp", regnum++, 1, "system", 32, "data_ptr");
  return regnum;
}
