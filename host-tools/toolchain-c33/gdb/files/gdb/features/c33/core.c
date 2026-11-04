/* THIS FILE IS GENERATED.  -*- buffer-read-only: t -*- vi:set ro:
  Original: core.xml */

#include "gdbsupport/tdesc.h"

static int
create_feature_c33_core (struct target_desc *result, long regnum)
{
  struct tdesc_feature *feature;

  feature = tdesc_create_feature (result, "org.gnu.gdb.c33.core");
  tdesc_type_with_fields *type_with_fields;
  type_with_fields = tdesc_create_flags (feature, "psr_flags", 4);
  tdesc_add_flag (type_with_fields, 0, "N");
  tdesc_add_flag (type_with_fields, 1, "Z");
  tdesc_add_flag (type_with_fields, 2, "V");
  tdesc_add_flag (type_with_fields, 3, "C");
  tdesc_add_flag (type_with_fields, 4, "IE");
  tdesc_type *field_type;
  field_type = tdesc_named_type (feature, "uint8");
  tdesc_add_typed_bitfield (type_with_fields, "IL", 8, 11, field_type);

  tdesc_create_reg (feature, "r0", regnum++, 1, "general", 32, "uint32");
  tdesc_create_reg (feature, "r1", regnum++, 1, "general", 32, "uint32");
  tdesc_create_reg (feature, "r2", regnum++, 1, "general", 32, "uint32");
  tdesc_create_reg (feature, "r3", regnum++, 1, "general", 32, "uint32");
  tdesc_create_reg (feature, "r4", regnum++, 1, "general", 32, "uint32");
  tdesc_create_reg (feature, "r5", regnum++, 1, "general", 32, "uint32");
  tdesc_create_reg (feature, "r6", regnum++, 1, "general", 32, "uint32");
  tdesc_create_reg (feature, "r7", regnum++, 1, "general", 32, "uint32");
  tdesc_create_reg (feature, "r8", regnum++, 1, "general", 32, "uint32");
  tdesc_create_reg (feature, "r9", regnum++, 1, "general", 32, "uint32");
  tdesc_create_reg (feature, "r10", regnum++, 1, "general", 32, "uint32");
  tdesc_create_reg (feature, "r11", regnum++, 1, "general", 32, "uint32");
  tdesc_create_reg (feature, "r12", regnum++, 1, "general", 32, "uint32");
  tdesc_create_reg (feature, "r13", regnum++, 1, "general", 32, "uint32");
  tdesc_create_reg (feature, "r14", regnum++, 1, "general", 32, "uint32");
  tdesc_create_reg (feature, "r15", regnum++, 1, "general", 32, "data_ptr");
  tdesc_create_reg (feature, "sp", regnum++, 1, "general", 32, "data_ptr");
  tdesc_create_reg (feature, "alr", regnum++, 1, "general", 32, "uint32");
  tdesc_create_reg (feature, "ahr", regnum++, 1, "general", 32, "uint32");
  tdesc_create_reg (feature, "psr", regnum++, 1, "general", 32, "psr_flags");
  tdesc_create_reg (feature, "pc", regnum++, 1, "general", 32, "code_ptr");
  return regnum;
}
