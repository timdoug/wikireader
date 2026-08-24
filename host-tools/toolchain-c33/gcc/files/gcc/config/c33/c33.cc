/* Subroutines for insn-output.cc for NEC C33 series
   Copyright (C) 1996-2026 Free Software Foundation, Inc.
   Contributed by Jeff Law (law@cygnus.com).

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

#define IN_TARGET_CODE 1

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "backend.h"
#include "target.h"
#include "rtl.h"
#include "tree.h"
#include "df.h"
#include "memmodel.h"
#include "tm_p.h"
#include "stringpool.h"
#include "attribs.h"
#include "insn-config.h"
#include "regs.h"
#include "emit-rtl.h"
#include "recog.h"
#include "diagnostic-core.h"
#include "stor-layout.h"
#include "varasm.h"
#include "calls.h"
#include "conditions.h"
#include "output.h"
#include "insn-attr.h"
#include "expr.h"
#include "cfgrtl.h"
#include "builtins.h"

/* This file should be included last.  */
#include "target-def.h"

#ifndef streq
#define streq(a,b) (strcmp (a, b) == 0)
#endif

static void c33_print_operand_address (FILE *, machine_mode, rtx);

/* Names of the various data areas used on the c33.  */
const char * GHS_default_section_names [(int) COUNT_OF_GHS_SECTION_KINDS];
const char * GHS_current_section_names [(int) COUNT_OF_GHS_SECTION_KINDS];

/* Track the current data area set by the data area pragma (which
   can be nested).  Tested by check_default_data_area.  */
data_area_stack_element * data_area_stack = NULL;

/* True if we don't need to check any more if the current
   function is an interrupt handler.  */
static int c33_interrupt_cache_p = FALSE;

/* Whether current function is an interrupt handler.  */
static int c33_interrupt_p = FALSE;

static GTY(()) section * rosdata_section;
static GTY(()) section * rozdata_section;
static GTY(()) section * tdata_section;
static GTY(()) section * zdata_section;
static GTY(()) section * zbss_section;

/* We use this to wrap all emitted insns in the prologue.  */
static rtx
F (rtx x)
{
  if (GET_CODE (x) != CLOBBER)
    RTX_FRAME_RELATED_P (x) = 1;
  return x;
}

/* Mark all the subexpressions of the PARALLEL rtx PAR as
   frame-related.  Return PAR.

   dwarf2out.cc:dwarf2out_frame_debug_expr ignores sub-expressions of a
   PARALLEL rtx other than the first if they do not have the
   FRAME_RELATED flag set on them.  */

static rtx
c33_all_frame_related (rtx par)
{
  int len = XVECLEN (par, 0);
  int i;

  gcc_assert (GET_CODE (par) == PARALLEL);
  for (i = 0; i < len; i++)
    F (XVECEXP (par, 0, i));

  return par;
}

/* Handle the TARGET_PASS_BY_REFERENCE target hook.
   Specify whether to pass the argument by reference.  */

static bool
c33_pass_by_reference (cumulative_args_t, const function_arg_info &arg)
{
  if (!TARGET_GCC_ABI)
    return 0;

  unsigned HOST_WIDE_INT size = arg.type_size_in_bytes ();
  return size > 8;
}

/* Return an RTX to represent where argument ARG will be passed to a function.
   If the result is NULL_RTX, the argument will be pushed.  */

static rtx
c33_function_arg (cumulative_args_t cum_v, const function_arg_info &arg)
{
  CUMULATIVE_ARGS *cum = get_cumulative_args (cum_v);
  rtx result = NULL_RTX;
  int size, align;

  if (!arg.named)
    return NULL_RTX;

  size = arg.promoted_size_in_bytes ();
  size = (size + UNITS_PER_WORD -1) & ~(UNITS_PER_WORD -1);

  if (size < 1)
    {
      /* Once we have stopped using argument registers, do not start up again.  */
      cum->nbytes = 4 * UNITS_PER_WORD;
      return NULL_RTX;
    }

  if (!TARGET_GCC_ABI)
    align = UNITS_PER_WORD;
  else if (size <= UNITS_PER_WORD && arg.type)
    align = TYPE_ALIGN (arg.type) / BITS_PER_UNIT;
  else
    align = size;

  cum->nbytes = (cum->nbytes + align - 1) &~(align - 1);

  if (cum->nbytes > 4 * UNITS_PER_WORD)
    return NULL_RTX;

  if (arg.type == NULL_TREE
      && cum->nbytes + size > 4 * UNITS_PER_WORD)
    return NULL_RTX;

  switch (cum->nbytes / UNITS_PER_WORD)
    {
    case 0:
      result = gen_rtx_REG (arg.mode, 6);
      break;
    case 1:
      result = gen_rtx_REG (arg.mode, 7);
      break;
    case 2:
      result = gen_rtx_REG (arg.mode, 8);
      break;
    case 3:
      result = gen_rtx_REG (arg.mode, 9);
      break;
    default:
      result = NULL_RTX;
    }

  return result;
}

/* Return the number of bytes which must be put into registers
   for values which are part in registers and part in memory.  */
static int
c33_arg_partial_bytes (cumulative_args_t cum_v, const function_arg_info &arg)
{
  CUMULATIVE_ARGS *cum = get_cumulative_args (cum_v);
  int size, align;

  if (!arg.named)
    return 0;

  size = arg.promoted_size_in_bytes ();
  if (size < 1)
    size = 1;

  if (!TARGET_GCC_ABI)
    align = UNITS_PER_WORD;
  else if (arg.type)
    align = TYPE_ALIGN (arg.type) / BITS_PER_UNIT;
  else
    align = size;

  cum->nbytes = (cum->nbytes + align - 1) & ~ (align - 1);

  if (cum->nbytes > 4 * UNITS_PER_WORD)
    return 0;

  if (cum->nbytes + size <= 4 * UNITS_PER_WORD)
    return 0;

  if (arg.type == NULL_TREE
      && cum->nbytes + size > 4 * UNITS_PER_WORD)
    return 0;

  return 4 * UNITS_PER_WORD - cum->nbytes;
}

/* Update the data in CUM to advance over argument ARG.  */

static void
c33_function_arg_advance (cumulative_args_t cum_v,
			   const function_arg_info &arg)
{
  CUMULATIVE_ARGS *cum = get_cumulative_args (cum_v);

  if (!TARGET_GCC_ABI)
    cum->nbytes += ((arg.promoted_size_in_bytes () + UNITS_PER_WORD - 1)
		    & -UNITS_PER_WORD);
  else
    cum->nbytes += (((arg.type && int_size_in_bytes (arg.type) > 8
		      ? GET_MODE_SIZE (Pmode)
		      : (HOST_WIDE_INT) arg.promoted_size_in_bytes ())
		     + UNITS_PER_WORD - 1)
		    & -UNITS_PER_WORD);
}

/* Return the high and low words of a CONST_DOUBLE */

static void
const_double_split (rtx x, HOST_WIDE_INT * p_high, HOST_WIDE_INT * p_low)
{
  if (GET_CODE (x) == CONST_DOUBLE)
    {
      long t[2];

      switch (GET_MODE (x))
	{
	case E_DFmode:
	  REAL_VALUE_TO_TARGET_DOUBLE (*CONST_DOUBLE_REAL_VALUE (x), t);
	  *p_high = t[1];	/* since c33 is little endian */
	  *p_low = t[0];	/* high is second word */
	  return;

	case E_SFmode:
	  REAL_VALUE_TO_TARGET_SINGLE (*CONST_DOUBLE_REAL_VALUE (x), *p_high);
	  *p_low = 0;
	  return;

	case E_VOIDmode:
	case E_DImode:
	  *p_high = CONST_DOUBLE_HIGH (x);
	  *p_low  = CONST_DOUBLE_LOW (x);
	  return;

	default:
	  break;
	}
    }

  fatal_insn ("const_double_split got a bad insn:", x);
}


/* Return the cost of the rtx R with code CODE.  */

static int
const_costs_int (HOST_WIDE_INT value, int zero_cost)
{
  if (CONST_OK_FOR_I (value))
      return zero_cost;
  else if (CONST_OK_FOR_J (value))
    return 1;
  else if (CONST_OK_FOR_K (value))
    return 2;
  else
    return 4;
}

static int
const_costs (rtx r, enum rtx_code c)
{
  HOST_WIDE_INT high, low;

  switch (c)
    {
    case CONST_INT:
      return const_costs_int (INTVAL (r), 0);

    case CONST_DOUBLE:
      const_double_split (r, &high, &low);
      if (GET_MODE (r) == SFmode)
	return const_costs_int (high, 1);
      else
	return const_costs_int (high, 1) + const_costs_int (low, 1);

    case SYMBOL_REF:
    case LABEL_REF:
    case CONST:
      return 2;

    case HIGH:
      return 1;

    default:
      return 4;
    }
}

static bool
c33_rtx_costs (rtx x, machine_mode mode, int outer_code,
		int opno ATTRIBUTE_UNUSED, int *total, bool speed)
{
  enum rtx_code code = GET_CODE (x);

  switch (code)
    {
    case CONST_INT:
    case CONST_DOUBLE:
    case CONST:
    case SYMBOL_REF:
    case LABEL_REF:
      *total = COSTS_N_INSNS (const_costs (x, code));
      return true;

    case MOD:
    case DIV:
    case UMOD:
    case UDIV:
      if (TARGET_C33E && !speed)
        *total = 6;
      else
	*total = 60;
      return true;

    case MULT:
      if (TARGET_C33E
	  && (mode == SImode || mode == HImode || mode == QImode))
        {
	  if (GET_CODE (XEXP (x, 1)) == REG)
	    *total = 4;
	  else if (GET_CODE (XEXP (x, 1)) == CONST_INT)
	    {
	      if (CONST_OK_FOR_O (INTVAL (XEXP (x, 1))))
	        *total = 6;
	      else if (CONST_OK_FOR_K (INTVAL (XEXP (x, 1))))
	        *total = 10;
	    }
        }
      else
	*total = 20;
      return true;

    case ZERO_EXTRACT:
      if (outer_code == COMPARE)
	*total = 0;
      return false;

    default:
      return false;
    }
}

/* Print operand X using operand code CODE to assembly language output file
   FILE.  */

static void
c33_print_operand (FILE * file, rtx x, int code)
{
  HOST_WIDE_INT high, low;

  switch (code)
    {
    case 'c':
      /* We use 'c' operands with symbols for .vtinherit.  */
      if (GET_CODE (x) == SYMBOL_REF)
        {
          output_addr_const(file, x);
          break;
        }
      /* Fall through.  */
    case 'b':
    case 'B':
    case 'C':
    case 'd':
    case 'D':
      switch ((code == 'B' || code == 'C' || code == 'D')
	      ? reverse_condition (GET_CODE (x)) : GET_CODE (x))
	{
	  case NE:
	    if (code == 'c' || code == 'C')
	      fprintf (file, "nz");
	    else
	      fprintf (file, "ne");
	    break;
	  case EQ:
	    if (code == 'c' || code == 'C')
	      fprintf (file, "z");
	    else
	      fprintf (file, "e");
	    break;
	  case GE:
	    if (code == 'D' || code == 'd')
	      fprintf (file, "p");
	    else
	      fprintf (file, "ge");
	    break;
	  case GT:
	    fprintf (file, "gt");
	    break;
	  case LE:
	    fprintf (file, "le");
	    break;
	  case LT:
	    if (code == 'D' || code == 'd')
	      fprintf (file, "n");
	    else
	      fprintf (file, "lt");
	    break;
	  case GEU:
	    fprintf (file, "nl");
	    break;
	  case GTU:
	    fprintf (file, "h");
	    break;
	  case LEU:
	    fprintf (file, "nh");
	    break;
	  case LTU:
	    fprintf (file, "l");
	    break;
	  default:
	    gcc_unreachable ();
	}
      break;
    case 'F':			/* High word of CONST_DOUBLE.  */
      switch (GET_CODE (x))
	{
	case CONST_INT:
	  fprintf (file, "%d", (INTVAL (x) >= 0) ? 0 : -1);
	  break;

	case CONST_DOUBLE:
	  const_double_split (x, &high, &low);
	  fprintf (file, "%ld", (long) high);
	  break;

	default:
	  gcc_unreachable ();
	}
      break;
    case 'G':			/* Low word of CONST_DOUBLE.  */
      switch (GET_CODE (x))
	{
	case CONST_INT:
	  fprintf (file, "%ld", (long) INTVAL (x));
	  break;

	case CONST_DOUBLE:
	  const_double_split (x, &high, &low);
	  fprintf (file, "%ld", (long) low);
	  break;

	default:
	  gcc_unreachable ();
	}
      break;
    case 'L':
      fprintf (file, "%d\n", (int)(INTVAL (x) & 0xffff));
      break;
    case 'M':
      fprintf (file, "%d", exact_log2 (INTVAL (x)));
      break;
    case 'O':
      gcc_assert (special_symbolref_operand (x, VOIDmode));

      if (GET_CODE (x) == CONST)
	x = XEXP (XEXP (x, 0), 0);
      else
	gcc_assert (GET_CODE (x) == SYMBOL_REF);

      if (SYMBOL_REF_ZDA_P (x))
	fprintf (file, "zdaoff");
      else if (SYMBOL_REF_SDA_P (x))
	fprintf (file, "sdaoff");
      else if (SYMBOL_REF_TDA_P (x))
	fprintf (file, "tdaoff");
      else
	gcc_unreachable ();
      break;
    case 'P':
      gcc_assert (special_symbolref_operand (x, VOIDmode));
      output_addr_const (file, x);
      break;
    case 'Q':
      gcc_assert (special_symbolref_operand (x, VOIDmode));

      if (GET_CODE (x) == CONST)
	x = XEXP (XEXP (x, 0), 0);
      else
	gcc_assert (GET_CODE (x) == SYMBOL_REF);

      if (SYMBOL_REF_ZDA_P (x))
	fprintf (file, "r0");
      else if (SYMBOL_REF_SDA_P (x))
	fprintf (file, "gp");
      else if (SYMBOL_REF_TDA_P (x))
	fprintf (file, "ep");
      else
	gcc_unreachable ();
      break;
    case 'R':		/* 2nd word of a double.  */
      switch (GET_CODE (x))
	{
	case REG:
	  fprintf (file, reg_names[REGNO (x) + 1]);
	  break;
	case MEM:
	  {
	    machine_mode mode = GET_MODE (x);
	    x = XEXP (adjust_address (x, SImode, 4), 0);
	    c33_print_operand_address (file, mode, x);
	    if (GET_CODE (x) == CONST_INT)
	      fprintf (file, "[r0]");
	  }
	  break;

	case CONST_INT:
	  {
	    unsigned HOST_WIDE_INT v = INTVAL (x);

	    /* Trickery to avoid problems with shifting
	       32-bits at a time on a 32-bit host.  */
	    v = v >> 16;
	    v = v >> 16;
	    fprintf (file, HOST_WIDE_INT_PRINT_HEX, v);
	    break;
	  }

	case CONST_DOUBLE:
	  fprintf (file, HOST_WIDE_INT_PRINT_HEX, CONST_DOUBLE_HIGH (x));
	  break;

	default:
	  debug_rtx (x);
	  gcc_unreachable ();
	}
      break;
    case 'S':
      {
        /* If it's a reference to a TDA variable, use sst/sld vs. st/ld.  */
        if (GET_CODE (x) == MEM && ep_memory_operand (x, GET_MODE (x), FALSE))
          fputs ("s", file);

        break;
      }
    case 'T':
      {
	/* Like an 'S' operand above, but for unsigned loads only.  */
        if (GET_CODE (x) == MEM && ep_memory_operand (x, GET_MODE (x), TRUE))
          fputs ("s", file);

        break;
      }
    case 'W':			/* Print the instruction suffix.  */
      switch (GET_MODE (x))
	{
	default:
	  gcc_unreachable ();

	case E_QImode: fputs (".b", file); break;
	case E_HImode: fputs (".h", file); break;
	case E_SImode: fputs (".w", file); break;
	case E_SFmode: fputs (".w", file); break;
	}
      break;
    case '.':			/* Register r0.  */
      fputs (reg_names[0], file);
      break;
    case 'z':			/* Reg or zero.  */
      if (REG_P (x))
	fputs (reg_names[REGNO (x)], file);
      else if ((GET_MODE(x) == SImode
		|| GET_MODE(x) == DFmode
		|| GET_MODE(x) == SFmode)
		&& x == CONST0_RTX(GET_MODE(x)))
      fputs (reg_names[0], file);
      else
	{
	  gcc_assert (x == const0_rtx);
	  fputs (reg_names[0], file);
	}
      break;
    default:
      switch (GET_CODE (x))
	{
	case MEM:
	  if (GET_CODE (XEXP (x, 0)) == CONST_INT)
	    output_address (GET_MODE (x),
			    gen_rtx_PLUS (SImode, gen_rtx_REG (SImode, 0),
					  XEXP (x, 0)));
	  else
	    output_address (GET_MODE (x), XEXP (x, 0));
	  break;

	case REG:
	  fputs (reg_names[REGNO (x)], file);
	  break;
	case SUBREG:
	  fputs (reg_names[subreg_regno (x)], file);
	  break;
	case CONST_DOUBLE:
	  fprintf (file, HOST_WIDE_INT_PRINT_HEX, CONST_DOUBLE_LOW (x));
	  break;

	case CONST_INT:
	case SYMBOL_REF:
	case CONST:
	case LABEL_REF:
	case CODE_LABEL:
	  c33_print_operand_address (file, VOIDmode, x);
	  break;
	default:
	  gcc_unreachable ();
	}
      break;

    }
}


/* Output assembly language output for the address ADDR to FILE.  */

static void
c33_print_operand_address (FILE * file, machine_mode /*mode*/, rtx addr)
{
  /* Addresses print *without* the surrounding brackets: every template that
     uses one writes them itself, as "ld.w %0,[%1]".  That is also how the
     3.3.2 backend did it.  The forms are [%rb], [%rb]+ and [%rb+disp]
     (core manual 5.5); the assembler supplies whatever ext prefixes the
     displacement needs.  */

  switch (GET_CODE (addr))
    {
    case REG:
    case SUBREG:
      c33_print_operand (file, addr, 0);
      break;

    case POST_INC:
      /* The trailing "+" belongs to the template, which writes "[%1]+".  */
      c33_print_operand (file, XEXP (addr, 0), 0);
      break;

    case PLUS:
      {
	rtx base = XEXP (addr, 0);
	rtx off = XEXP (addr, 1);

	/* Canonical form is (plus reg disp), but accept the reverse.  */
	if (CONST_INT_P (base))
	  std::swap (base, off);

	c33_print_operand (file, base, 0);
	if (!CONST_INT_P (off) || INTVAL (off) >= 0)
	  fprintf (file, "+");
	c33_print_operand (file, off, 0);
      }
      break;

    default:
      output_addr_const (file, addr);
      break;
    }
}

static bool
c33_print_operand_punct_valid_p (unsigned char code)
{
  return code == '.';
}

/* When assemble_integer is used to emit the offsets for a switch
   table it can encounter (TRUNCATE:HI (MINUS:SI (LABEL_REF:SI) (LABEL_REF:SI))).
   output_addr_const will normally barf at this, but it is OK to omit
   the truncate and just emit the difference of the two labels.  The
   .hword directive will automatically handle the truncation for us.

   Returns true if rtx was handled, false otherwise.  */

static bool
c33_output_addr_const_extra (FILE * file, rtx x)
{
  if (GET_CODE (x) != TRUNCATE)
    return false;

  x = XEXP (x, 0);

  /* We must also handle the case where the switch table was passed a
     constant value and so has been collapsed.  In this case the first
     label will have been deleted.  In such a case it is OK to emit
     nothing, since the table will not be used.
     (cf gcc.c-torture/compile/990801-1.c).  */
  if (GET_CODE (x) == MINUS
      && GET_CODE (XEXP (x, 0)) == LABEL_REF)
    {
      rtx_code_label *label
	= dyn_cast<rtx_code_label *> (XEXP (XEXP (x, 0), 0));
      if (label && label->deleted ())
	return true;
    }

  output_addr_const (file, x);
  return true;
}

/* Return appropriate code to load up a 1, 2, or 4 integer/floating
   point value.  */

const char *
output_move_single (rtx * operands)
{
  rtx dst = operands[0];
  rtx src = operands[1];

  /* The C33 has one move instruction, ld, in a size suffixed form: ld.w,
     ld.h, ld.b and the unsigned ld.uh/ld.ub (core manual 5.5).  Destination
     comes first, so a register copy is "ld.w %rd,%rs" -- the reverse of the
     V850's "mov %rs,%rd".

     The x-prefixed spelling (xld.w) tells the assembler it may emit ext
     prefixes, which is how a 32-bit immediate or a displaced address is
     reached.  We use it wherever the operand is not provably short; the
     assembler picks the narrowest encoding that works.  */

  if (REG_P (dst))
    {
      if (REG_P (src))
	return "ld%W0\t%0,%1";

      if (CONST_INT_P (src))
	return "xld.w\t%0,%1";

      if (GET_CODE (src) == CONST_DOUBLE && GET_MODE (src) == SFmode)
	return "xld.w\t%0,%F1";

      if (MEM_P (src))
	{
	  rtx addr = XEXP (src, 0);

	  if (GET_CODE (addr) == POST_INC)
	    return "ld%W1\t%0,[%1]+";
	  if (REG_P (addr) || SUBREG_P (addr))
	    return "ld%W1\t%0,[%1]";
	  /* base+displacement, or an absolute address.  */
	  return "xld%W1\t%0,[%1]";
	}

      if (GET_CODE (src) == LABEL_REF
	  || GET_CODE (src) == SYMBOL_REF
	  || GET_CODE (src) == CONST)
	/* XXX Taking the address of a symbol.  This is correct only with
	   -medda32; the %r15-relative default data area form belongs to the
	   data-area work (step 5 in README.md), which has to decide between
	   "ext doff_hi(sym); ext doff_lo(sym); add %rd,%r15" and this.  */
	return "xld.w\t%0,%1";
    }
  else if (MEM_P (dst))
    {
      rtx addr = XEXP (dst, 0);

      if (REG_P (src))
	{
	  if (GET_CODE (addr) == POST_INC)
	    return "ld%W0\t[%0]+,%1";
	  if (REG_P (addr) || SUBREG_P (addr))
	    return "ld%W0\t[%0],%1";
	  return "xld%W0\t[%0],%1";
	}

      /* There is no hardwired zero register, so a store of 0 has to go
	 through a register; the movsi expander forces that.  */
    }

  fatal_insn ("output_move_single:", gen_rtx_SET (dst, src));
  return "";
}

machine_mode
c33_select_cc_mode (enum rtx_code cond, rtx op0, rtx op1)
{
  /* The C33 has no FPU; floating point is done in libgcc, so a float
     comparison never reaches here as a machine comparison.  */
  gcc_assert (GET_MODE_CLASS (GET_MODE (op0)) != MODE_FLOAT);

  if (op1 == const0_rtx
      && (cond == EQ || cond == NE || cond == LT || cond == GE)
      && (GET_CODE (op0) == PLUS || GET_CODE (op0) == MINUS
	  || GET_CODE (op0) == NEG || GET_CODE (op0) == AND
	  || GET_CODE (op0) == IOR || GET_CODE (op0) == XOR
	  || GET_CODE (op0) == NOT || GET_CODE (op0) == ASHIFT))
    return CCNZmode;

  return CCmode;
}


/* Return maximum offset supported for a short EP memory reference of mode
   MODE and signedness UNSIGNEDP.  */

static int
ep_memory_offset (machine_mode mode, int unsignedp ATTRIBUTE_UNUSED)
{
  int max_offset = 0;

  switch (mode)
    {
    case E_QImode:
      if (TARGET_SMALL_SLD)
	max_offset = (1 << 4);
      else if ((TARGET_C33E_UP)
		&& unsignedp)
	max_offset = (1 << 4);
      else
	max_offset = (1 << 7);
      break;

    case E_HImode:
      if (TARGET_SMALL_SLD)
	max_offset = (1 << 5);
      else if ((TARGET_C33E_UP)
		&& unsignedp)
	max_offset = (1 << 5);
      else
	max_offset = (1 << 8);
      break;

    case E_SImode:
    case E_SFmode:
      max_offset = (1 << 8);
      break;

    default:
      break;
    }

  return max_offset;
}

/* Return true if OP is a valid short EP memory reference */

int
ep_memory_operand (rtx op, machine_mode mode, int unsigned_load)
{
  rtx addr, op0, op1;
  int max_offset;
  int mask;

  /* XXX Disabled.  This is V850's ep-relative short addressing.  The C33
     analogue is %r15-relative default-data-area addressing, but %r15 is a
     fixed register, so leaving this enabled makes reload try to load
     addresses into a register it may not allocate -- which shows up as an
     endless chain of reload copies.  Re-enable as part of the data area
     work (step 5 in README.md), driven by a proper address predicate
     rather than this predicate.  */
  return FALSE;

  /* If we are not using the EP register on a per-function basis
     then do not allow this optimization at all.  This is to
     prevent the use of the SLD/SST instructions which cannot be
     guaranteed to work properly due to a hardware bug.  */
  if (!TARGET_EP)
    return FALSE;

  if (GET_CODE (op) != MEM)
    return FALSE;

  max_offset = ep_memory_offset (mode, unsigned_load);

  mask = GET_MODE_SIZE (mode) - 1;

  addr = XEXP (op, 0);
  if (GET_CODE (addr) == CONST)
    addr = XEXP (addr, 0);

  switch (GET_CODE (addr))
    {
    default:
      break;

    case SYMBOL_REF:
      return SYMBOL_REF_TDA_P (addr);

    case REG:
      return REGNO (addr) == C33_DP_REGNUM;

    case PLUS:
      op0 = XEXP (addr, 0);
      op1 = XEXP (addr, 1);
      if (GET_CODE (op1) == CONST_INT
	  && INTVAL (op1) < max_offset
	  && INTVAL (op1) >= 0
	  && (INTVAL (op1) & mask) == 0)
	{
	  if (GET_CODE (op0) == REG && REGNO (op0) == C33_DP_REGNUM)
	    return TRUE;

	  if (GET_CODE (op0) == SYMBOL_REF && SYMBOL_REF_TDA_P (op0))
	    return TRUE;
	}
      break;
    }

  return FALSE;
}

/* Substitute memory references involving a pointer, to use the ep pointer,
   taking care to save and preserve the ep.  */

static void
substitute_ep_register (rtx_insn *first_insn,
                        rtx_insn *last_insn,
                        int uses,
                        int regno,
                        rtx * p_r1,
                        rtx * p_ep)
{
  rtx reg = gen_rtx_REG (Pmode, regno);
  rtx_insn *insn;

  if (!*p_r1)
    {
      df_set_regs_ever_live (1, true);
      *p_r1 = gen_rtx_REG (Pmode, 1);
      *p_ep = gen_rtx_REG (Pmode, 30);
    }

  if (TARGET_DEBUG)
    fprintf (stderr, "\
Saved %d bytes (%d uses of register %s) in function %s, starting as insn %d, ending at %d\n",
	     2 * (uses - 3), uses, reg_names[regno],
	     IDENTIFIER_POINTER (DECL_NAME (current_function_decl)),
	     INSN_UID (first_insn), INSN_UID (last_insn));

  if (NOTE_P (first_insn))
    first_insn = next_nonnote_insn (first_insn);

  last_insn = next_nonnote_insn (last_insn);
  for (insn = first_insn; insn && insn != last_insn; insn = NEXT_INSN (insn))
    {
      if (NONJUMP_INSN_P (insn))
	{
	  rtx pattern = single_set (insn);

	  /* Replace the memory references.  */
	  if (pattern)
	    {
	      rtx *p_mem;
	      /* Memory operands are signed by default.  */
	      int unsignedp = FALSE;

	      if (GET_CODE (SET_DEST (pattern)) == MEM
		  && GET_CODE (SET_SRC (pattern)) == MEM)
		p_mem = (rtx *)0;

	      else if (GET_CODE (SET_DEST (pattern)) == MEM)
		p_mem = &SET_DEST (pattern);

	      else if (GET_CODE (SET_SRC (pattern)) == MEM)
		p_mem = &SET_SRC (pattern);

	      else if (GET_CODE (SET_SRC (pattern)) == SIGN_EXTEND
		       && GET_CODE (XEXP (SET_SRC (pattern), 0)) == MEM)
		p_mem = &XEXP (SET_SRC (pattern), 0);

	      else if (GET_CODE (SET_SRC (pattern)) == ZERO_EXTEND
		       && GET_CODE (XEXP (SET_SRC (pattern), 0)) == MEM)
		{
		  p_mem = &XEXP (SET_SRC (pattern), 0);
		  unsignedp = TRUE;
		}
	      else
		p_mem = (rtx *)0;

	      if (p_mem)
		{
		  rtx addr = XEXP (*p_mem, 0);

		  if (GET_CODE (addr) == REG && REGNO (addr) == (unsigned) regno)
		    *p_mem = change_address (*p_mem, VOIDmode, *p_ep);

		  else if (GET_CODE (addr) == PLUS
			   && GET_CODE (XEXP (addr, 0)) == REG
			   && REGNO (XEXP (addr, 0)) == (unsigned) regno
			   && GET_CODE (XEXP (addr, 1)) == CONST_INT
			   && ((INTVAL (XEXP (addr, 1)))
			       < ep_memory_offset (GET_MODE (*p_mem),
						   unsignedp))
			   && ((INTVAL (XEXP (addr, 1))) >= 0))
		    *p_mem = change_address (*p_mem, VOIDmode,
					     gen_rtx_PLUS (Pmode,
							   *p_ep,
							   XEXP (addr, 1)));
		}
	    }
	}
    }

  /* Optimize back to back cases of ep <- r1 & r1 <- ep.  */
  insn = prev_nonnote_insn (first_insn);
  if (insn && NONJUMP_INSN_P (insn)
      && GET_CODE (PATTERN (insn)) == SET
      && SET_DEST (PATTERN (insn)) == *p_ep
      && SET_SRC (PATTERN (insn)) == *p_r1)
    delete_insn (insn);
  else
    emit_insn_before (gen_rtx_SET (*p_r1, *p_ep), first_insn);

  emit_insn_before (gen_rtx_SET (*p_ep, reg), first_insn);
  emit_insn_before (gen_rtx_SET (*p_ep, *p_r1), last_insn);
}


/* TARGET_MACHINE_DEPENDENT_REORG.  On the 850, we use it to implement
   the -mep mode to copy heavily used pointers to ep to use the implicit
   addressing.  */

static void
c33_reorg (void)
{
  struct
  {
    int uses;
    rtx_insn *first_insn;
    rtx_insn *last_insn;
  }
  regs[FIRST_PSEUDO_REGISTER];

  int i;
  int use_ep = FALSE;
  rtx r1 = NULL_RTX;
  rtx ep = NULL_RTX;
  rtx_insn *insn;
  rtx pattern;

  /* If not ep mode, just return now.  */
  if (!TARGET_EP)
    return;

  for (i = 0; i < FIRST_PSEUDO_REGISTER; i++)
    {
      regs[i].uses = 0;
      regs[i].first_insn = NULL;
      regs[i].last_insn = NULL;
    }

  for (insn = get_insns (); insn != NULL_RTX; insn = NEXT_INSN (insn))
    {
      switch (GET_CODE (insn))
	{
	  /* End of basic block */
	default:
	  if (!use_ep)
	    {
	      int max_uses = -1;
	      int max_regno = -1;

	      for (i = 0; i < FIRST_PSEUDO_REGISTER; i++)
		{
		  if (max_uses < regs[i].uses)
		    {
		      max_uses = regs[i].uses;
		      max_regno = i;
		    }
		}

	      if (max_uses > 3)
		substitute_ep_register (regs[max_regno].first_insn,
					regs[max_regno].last_insn,
					max_uses, max_regno, &r1, &ep);
	    }

	  use_ep = FALSE;
	  for (i = 0; i < FIRST_PSEUDO_REGISTER; i++)
	    {
	      regs[i].uses = 0;
	      regs[i].first_insn = NULL;
	      regs[i].last_insn = NULL;
	    }
	  break;

	case NOTE:
	  break;

	case INSN:
	  pattern = single_set (insn);

	  /* See if there are any memory references we can shorten.  */
	  if (pattern)
	    {
	      rtx src = SET_SRC (pattern);
	      rtx dest = SET_DEST (pattern);
	      rtx mem;
	      /* Memory operands are signed by default.  */
	      int unsignedp = FALSE;

	      /* We might have (SUBREG (MEM)) here, so just get rid of the
		 subregs to make this code simpler.  */
	      if (GET_CODE (dest) == SUBREG
		  && (GET_CODE (SUBREG_REG (dest)) == MEM
		      || GET_CODE (SUBREG_REG (dest)) == REG))
		alter_subreg (&dest, false);
	      if (GET_CODE (src) == SUBREG
		  && (GET_CODE (SUBREG_REG (src)) == MEM
		      || GET_CODE (SUBREG_REG (src)) == REG))
		alter_subreg (&src, false);

	      if (GET_CODE (dest) == MEM && GET_CODE (src) == MEM)
		mem = NULL_RTX;

	      else if (GET_CODE (dest) == MEM)
		mem = dest;

	      else if (GET_CODE (src) == MEM)
		mem = src;

	      else if (GET_CODE (src) == SIGN_EXTEND
		       && GET_CODE (XEXP (src, 0)) == MEM)
		mem = XEXP (src, 0);

	      else if (GET_CODE (src) == ZERO_EXTEND
		       && GET_CODE (XEXP (src, 0)) == MEM)
		{
		  mem = XEXP (src, 0);
		  unsignedp = TRUE;
		}
	      else
		mem = NULL_RTX;

	      if (mem && ep_memory_operand (mem, GET_MODE (mem), unsignedp))
		use_ep = TRUE;

	      else if (!use_ep && mem
		       && GET_MODE_SIZE (GET_MODE (mem)) <= UNITS_PER_WORD)
		{
		  rtx addr = XEXP (mem, 0);
		  int regno = -1;
		  int short_p;

		  if (GET_CODE (addr) == REG)
		    {
		      short_p = TRUE;
		      regno = REGNO (addr);
		    }

		  else if (GET_CODE (addr) == PLUS
			   && GET_CODE (XEXP (addr, 0)) == REG
			   && GET_CODE (XEXP (addr, 1)) == CONST_INT
			   && ((INTVAL (XEXP (addr, 1)))
			       < ep_memory_offset (GET_MODE (mem), unsignedp))
			   && ((INTVAL (XEXP (addr, 1))) >= 0))
		    {
		      short_p = TRUE;
		      regno = REGNO (XEXP (addr, 0));
		    }

		  else
		    short_p = FALSE;

		  if (short_p)
		    {
		      regs[regno].uses++;
		      regs[regno].last_insn = insn;
		      if (!regs[regno].first_insn)
			regs[regno].first_insn = insn;
		    }
		}

	      /* Loading up a register in the basic block zaps any savings
		 for the register */
	      if (GET_CODE (dest) == REG)
		{
		  int regno;
		  int endregno;

		  regno = REGNO (dest);
		  endregno = END_REGNO (dest);

		  if (!use_ep)
		    {
		      /* See if we can use the pointer before this
			 modification.  */
		      int max_uses = -1;
		      int max_regno = -1;

		      for (i = 0; i < FIRST_PSEUDO_REGISTER; i++)
			{
			  if (max_uses < regs[i].uses)
			    {
			      max_uses = regs[i].uses;
			      max_regno = i;
			    }
			}

		      if (max_uses > 3
			  && max_regno >= regno
			  && max_regno < endregno)
			{
			  substitute_ep_register (regs[max_regno].first_insn,
						  regs[max_regno].last_insn,
						  max_uses, max_regno, &r1,
						  &ep);

			  /* Since we made a substitution, zap all remembered
			     registers.  */
			  for (i = 0; i < FIRST_PSEUDO_REGISTER; i++)
			    {
			      regs[i].uses = 0;
			      regs[i].first_insn = NULL;
			      regs[i].last_insn = NULL;
			    }
			}
		    }

		  for (i = regno; i < endregno; i++)
		    {
		      regs[i].uses = 0;
		      regs[i].first_insn = NULL;
		      regs[i].last_insn = NULL;
		    }
		}
	    }
	}
    }
}

/* # of registers saved by the interrupt handler.  */
#define INTERRUPT_FIXED_NUM 5

/* # of bytes for registers saved by the interrupt handler.  */
#define INTERRUPT_FIXED_SAVE_SIZE (4 * INTERRUPT_FIXED_NUM)

/* # of words saved for other registers.  */
#define INTERRUPT_ALL_SAVE_NUM \
  (30 - INTERRUPT_FIXED_NUM)

#define INTERRUPT_ALL_SAVE_SIZE (4 * INTERRUPT_ALL_SAVE_NUM)

/* Compute how much stack the callee-saved registers need, and which ones
   they are.

   The C33 saves registers with push/pushn, which grow the stack downwards
   by 4 per register (core manual 2.4.2).  pushn %rN pushes the whole block
   %r0..%rN, so if any callee-saved register is live we save the contiguous
   run %r0..%rHIGHEST -- pushing a couple of extra words is cheaper than the
   individual pushes it would take to skip the gaps.

   Only %r0-%r3 are callee-saved (see ABI.md).  An interrupt handler must
   additionally preserve everything it touches, because it can interrupt
   anything.  */

int
compute_register_save_size (long * p_reg_saved)
{
  int i;
  int size = 0;
  long reg_saved = 0;
  int interrupt_handler = c33_interrupt_function_p (current_function_decl);
  int highest = -1;

  if (interrupt_handler)
    {
      /* Preserve every general register the handler actually uses, plus
	 anything a callee of the handler might clobber.  */
      int calls_others = !crtl->is_leaf;

      for (i = 0; i < 16; i++)
	if (df_regs_ever_live_p (i) || (calls_others && call_used_or_fixed_reg_p (i)))
	  highest = i;
    }
  else
    {
      for (i = C33_FIRST_SAVED_REG; i <= C33_LAST_SAVED_REG; i++)
	if (df_regs_ever_live_p (i) && ! call_used_or_fixed_reg_p (i))
	  highest = i;

      /* A frame pointer lives in %r3 and must be preserved too.  */
      if (frame_pointer_needed && highest < HARD_FRAME_POINTER_REGNUM)
	highest = HARD_FRAME_POINTER_REGNUM;
    }

  for (i = 0; i <= highest; i++)
    {
      reg_saved |= 1L << i;
      size += 4;
    }

  if (p_reg_saved)
    *p_reg_saved = reg_saved;

  return size;
}

/* The largest %sp adjustment a single add/sub %sp,imm10 can make.  The
   immediate is 10 bits scaled by 4, and the form cannot be extended with
   ext (core manual p64), so anything larger needs a scratch register.  */
#define C33_MAX_SP_ADJUST (1023 * 4)

/* Add DELTA to the stack pointer.  DELTA is a byte count; negative grows
   the stack.  Emits the add/sub %sp,imm10 form when it fits, otherwise
   materialises the amount in a scratch register.  */

static void
c33_adjust_sp (HOST_WIDE_INT delta, bool frame_related)
{
  rtx insn;

  if (delta == 0)
    return;

  if (IN_RANGE (delta, -C33_MAX_SP_ADJUST, C33_MAX_SP_ADJUST)
      && (delta & 3) == 0)
    insn = emit_insn (gen_add_sp_imm (GEN_INT (delta)));
  else
    /* Out of reach of add/sub %sp,imm10, which cannot be ext-extended.  Go
       through %r14, which is call-clobbered and dead at prologue/epilogue
       time; the pattern clobbers it explicitly.  */
    insn = emit_insn (gen_add_sp_big (GEN_INT (delta)));

  if (frame_related)
    {
      RTX_FRAME_RELATED_P (insn) = 1;
      add_reg_note (insn, REG_FRAME_RELATED_EXPR,
		    gen_rtx_SET (stack_pointer_rtx,
				 plus_constant (Pmode, stack_pointer_rtx,
						delta)));
    }
}

void
expand_prologue (void)
{
  long reg_saved = 0;
  unsigned int size = get_frame_size ();
  int actual_fsize = compute_frame_size (size, &reg_saved);
  int save_size = compute_register_save_size (NULL);
  int highest = -1;
  int i;

  if (flag_stack_usage_info)
    current_function_static_stack_size = actual_fsize;

  for (i = 0; i < 16; i++)
    if (reg_saved & (1L << i))
      highest = i;

  /* Save the callee-saved block with a single pushn %rHIGHEST.  This both
     stores the registers and moves %sp down by 4 * (highest + 1).  */
  if (highest >= 0)
    {
      rtx insn = emit_insn (gen_pushn (GEN_INT (highest)));
      RTX_FRAME_RELATED_P (insn) = 1;
    }

  /* Then carve out the local frame and any outgoing argument area.  */
  c33_adjust_sp (-(actual_fsize - save_size), true);

  if (frame_pointer_needed)
    {
      rtx insn = emit_move_insn (hard_frame_pointer_rtx, stack_pointer_rtx);
      RTX_FRAME_RELATED_P (insn) = 1;
    }
}

void
expand_epilogue (void)
{
  long reg_saved = 0;
  unsigned int size = get_frame_size ();
  int actual_fsize = compute_frame_size (size, &reg_saved);
  int save_size = compute_register_save_size (NULL);
  int highest = -1;
  int i;

  for (i = 0; i < 16; i++)
    if (reg_saved & (1L << i))
      highest = i;

  /* If a frame pointer was set up, recover %sp from it: the frame may have
     been extended dynamically by alloca.  */
  if (frame_pointer_needed)
    emit_move_insn (stack_pointer_rtx, hard_frame_pointer_rtx);
  else
    c33_adjust_sp (actual_fsize - save_size, false);

  if (highest >= 0)
    emit_insn (gen_popn (GEN_INT (highest)));

  /* ret pops the return address that call pushed (core manual 2.4.4);
     there is no link register to jump through.  An interrupt pushed PSR as
     well, so a handler has to leave through reti instead.  */
  if (c33_interrupt_function_p (current_function_decl))
    emit_jump_insn (gen_return_interrupt ());
  else
    emit_jump_insn (gen_return_internal ());
}

/* Typical stack layout should looks like this after the function's prologue:

                            |    |
                              --                       ^
                            |    | \                   |
                            |    |   arguments saved   | Increasing
                            |    |   on the stack      |  addresses
    PARENT   arg pointer -> |    | /
  -------------------------- ---- -------------------
                            |    | - space for argument split between regs & stack
			      --
    CHILD                   |    | \    <-- (return address here)
                            |    |   other call
                            |    |   saved registers
                            |    | /
                              --
        frame pointer ->    |    | \             ___
                            |    |   local        |
                            |    |   variables    |f
                            |    | /              |r
                              --                  |a
                            |    | \              |m
                            |    |   outgoing     |e
                            |    |   arguments    |    | Decreasing
    (hard) frame pointer    |    |  /             |    |  addresses
       and stack pointer -> |    | /             _|_   |
  -------------------------- ---- ------------------   V */

int
compute_frame_size (poly_int64 size, long * p_reg_saved)
{
  return (size
	  + compute_register_save_size (p_reg_saved)
	  + crtl->outgoing_args_size);
}

static int
use_prolog_function (int num_save, int frame_size)
{
  int alloc_stack = (4 * num_save);
  int unalloc_stack = frame_size - alloc_stack;
  int save_func_len, restore_func_len;
  int save_normal_len, restore_normal_len;

  if (! TARGET_DISABLE_CALLT)
      save_func_len = restore_func_len = 2;
  else
      save_func_len = restore_func_len = TARGET_LONG_CALLS ? (4+4+4+2+2) : 4;

  if (unalloc_stack)
    {
      save_func_len += CONST_OK_FOR_J (-unalloc_stack) ? 2 : 4;
      restore_func_len += CONST_OK_FOR_J (-unalloc_stack) ? 2 : 4;
    }

  /* See if we would have used ep to save the stack.  */
  if (TARGET_EP && num_save > 3 && (unsigned)frame_size < 255)
    save_normal_len = restore_normal_len = (3 * 2) + (2 * num_save);
  else
    save_normal_len = restore_normal_len = 4 * num_save;

  save_normal_len += CONST_OK_FOR_J (-frame_size) ? 2 : 4;
  restore_normal_len += (CONST_OK_FOR_J (frame_size) ? 2 : 4) + 2;

  /* Don't bother checking if we don't actually save any space.
     This happens for instance if one register is saved and additional
     stack space is allocated.  */
  return ((save_func_len + restore_func_len) < (save_normal_len + restore_normal_len));
}

static void
increment_stack (signed int amount, bool in_prologue)
{
  rtx inc;

  if (amount == 0)
    return;

  inc = GEN_INT (amount);

  if (! CONST_OK_FOR_K (amount))
    {
      rtx reg = gen_rtx_REG (Pmode, 12);

      inc = emit_move_insn (reg, inc);
      if (in_prologue)
	F (inc);
      inc = reg;
    }

  inc = emit_insn (gen_addsi3_clobber_flags (stack_pointer_rtx, stack_pointer_rtx, inc));
  if (in_prologue)
    F (inc);
}




/* Retrieve the data area that has been chosen for the given decl.  */

c33_data_area
c33_get_data_area (tree decl)
{
  if (lookup_attribute ("sda", DECL_ATTRIBUTES (decl)) != NULL_TREE)
    return DATA_AREA_SDA;

  if (lookup_attribute ("tda", DECL_ATTRIBUTES (decl)) != NULL_TREE)
    return DATA_AREA_TDA;

  if (lookup_attribute ("zda", DECL_ATTRIBUTES (decl)) != NULL_TREE)
    return DATA_AREA_ZDA;

  return DATA_AREA_NORMAL;
}

/* Store the indicated data area in the decl's attributes.  */

static void
c33_set_data_area (tree decl, c33_data_area data_area)
{
  tree name;

  switch (data_area)
    {
    case DATA_AREA_SDA: name = get_identifier ("sda"); break;
    case DATA_AREA_TDA: name = get_identifier ("tda"); break;
    case DATA_AREA_ZDA: name = get_identifier ("zda"); break;
    default:
      return;
    }

  DECL_ATTRIBUTES (decl) = tree_cons
    (name, NULL, DECL_ATTRIBUTES (decl));
}

/* Handle an "interrupt" attribute; arguments as in
   struct attribute_spec.handler.  */
static tree
c33_handle_interrupt_attribute (tree *node, tree name,
                                 tree args ATTRIBUTE_UNUSED,
                                 int flags ATTRIBUTE_UNUSED,
                                 bool * no_add_attrs)
{
  if (TREE_CODE (*node) != FUNCTION_DECL)
    {
      warning (OPT_Wattributes, "%qE attribute only applies to functions",
	       name);
      *no_add_attrs = true;
    }

  return NULL_TREE;
}

/* Handle a "sda", "tda" or "zda" attribute; arguments as in
   struct attribute_spec.handler.  */
static tree
c33_handle_data_area_attribute (tree *node, tree name,
                                 tree args ATTRIBUTE_UNUSED,
                                 int flags ATTRIBUTE_UNUSED,
                                 bool * no_add_attrs)
{
  c33_data_area data_area;
  c33_data_area area;
  tree decl = *node;

  /* Implement data area attribute.  */
  if (is_attribute_p ("sda", name))
    data_area = DATA_AREA_SDA;
  else if (is_attribute_p ("tda", name))
    data_area = DATA_AREA_TDA;
  else if (is_attribute_p ("zda", name))
    data_area = DATA_AREA_ZDA;
  else
    gcc_unreachable ();

  switch (TREE_CODE (decl))
    {
    case VAR_DECL:
      if (current_function_decl != NULL_TREE)
	{
          error_at (DECL_SOURCE_LOCATION (decl),
		    "data area attributes cannot be specified for "
		    "local variables");
	  *no_add_attrs = true;
	}

      /* FALLTHRU */

    case FUNCTION_DECL:
      area = c33_get_data_area (decl);
      if (area != DATA_AREA_NORMAL && data_area != area)
	{
	  error ("data area of %q+D conflicts with previous declaration",
                 decl);
	  *no_add_attrs = true;
	}
      break;

    default:
      break;
    }

  return NULL_TREE;
}


/* Return nonzero if FUNC is an interrupt function as specified
   by the "interrupt" attribute.  */

int
c33_interrupt_function_p (tree func)
{
  tree a;
  int ret = 0;

  if (c33_interrupt_cache_p)
    return c33_interrupt_p;

  if (TREE_CODE (func) != FUNCTION_DECL)
    return 0;

  a = lookup_attribute ("interrupt_handler", DECL_ATTRIBUTES (func));
  if (a != NULL_TREE)
    ret = 1;

  else
    {
      a = lookup_attribute ("interrupt", DECL_ATTRIBUTES (func));
      ret = a != NULL_TREE;
    }

  /* Its not safe to trust global variables until after function inlining has
     been done.  */
  if (reload_completed | reload_in_progress)
    c33_interrupt_p = ret;

  return ret;
}


static void
c33_encode_data_area (tree decl, rtx symbol)
{
  int flags;

  /* Map explicit sections into the appropriate attribute */
  if (c33_get_data_area (decl) == DATA_AREA_NORMAL)
    {
      if (DECL_SECTION_NAME (decl))
	{
	  const char *name = DECL_SECTION_NAME (decl);

	  if (streq (name, ".zdata") || streq (name, ".zbss"))
	    c33_set_data_area (decl, DATA_AREA_ZDA);

	  else if (streq (name, ".sdata") || streq (name, ".sbss"))
	    c33_set_data_area (decl, DATA_AREA_SDA);

	  else if (streq (name, ".tdata"))
	    c33_set_data_area (decl, DATA_AREA_TDA);
	}

      /* If no attribute, support -m{zda,sda,tda}=n */
      else
	{
	  int size = int_size_in_bytes (TREE_TYPE (decl));
	  if (size <= 0)
	    ;

	  else if (size <= small_memory_max [(int) SMALL_MEMORY_TDA])
	    c33_set_data_area (decl, DATA_AREA_TDA);

	  else if (size <= small_memory_max [(int) SMALL_MEMORY_SDA])
	    c33_set_data_area (decl, DATA_AREA_SDA);

	  else if (size <= small_memory_max [(int) SMALL_MEMORY_ZDA])
	    c33_set_data_area (decl, DATA_AREA_ZDA);
	}

      if (c33_get_data_area (decl) == DATA_AREA_NORMAL)
	return;
    }

  flags = SYMBOL_REF_FLAGS (symbol);
  switch (c33_get_data_area (decl))
    {
    case DATA_AREA_ZDA: flags |= SYMBOL_FLAG_ZDA; break;
    case DATA_AREA_TDA: flags |= SYMBOL_FLAG_TDA; break;
    case DATA_AREA_SDA: flags |= SYMBOL_FLAG_SDA; break;
    default: gcc_unreachable ();
    }
  SYMBOL_REF_FLAGS (symbol) = flags;
}

static void
c33_encode_section_info (tree decl, rtx rtl, int first)
{
  default_encode_section_info (decl, rtl, first);

  if (VAR_P (decl)
      && (TREE_STATIC (decl) || DECL_EXTERNAL (decl)))
    c33_encode_data_area (decl, XEXP (rtl, 0));
}

/* Construct a JR instruction to a routine that will perform the equivalent of
   the RTL passed in as an argument.  This RTL is a function epilogue that
   pops registers off the stack and possibly releases some extra stack space
   as well.  The code has already verified that the RTL matches these
   requirements.  */



/* Construct a JARL instruction to a routine that will perform the equivalent
   of the RTL passed as a parameter.  This RTL is a function prologue that
   saves some of the registers r20 - r31 onto the stack, and possibly acquires
   some stack space as well.  The code has already verified that the RTL
   matches these requirements.  */

/* A version of asm_output_aligned_bss() that copes with the special
   data areas of the c33.  */
void
c33_output_aligned_bss (FILE * file,
                         tree decl,
                         const char * name,
                         unsigned HOST_WIDE_INT size,
                         int align)
{
  switch (c33_get_data_area (decl))
    {
    case DATA_AREA_ZDA:
      switch_to_section (zbss_section);
      break;

    case DATA_AREA_SDA:
      switch_to_section (sbss_section);
      break;

    case DATA_AREA_TDA:
      switch_to_section (tdata_section);
      break;

    default:
      switch_to_section (bss_section);
      break;
    }

  ASM_OUTPUT_ALIGN (file, floor_log2 (align / BITS_PER_UNIT));
#ifdef ASM_DECLARE_OBJECT_NAME
  last_assemble_variable_decl = decl;
  ASM_DECLARE_OBJECT_NAME (file, name, decl);
#else
  /* Standard thing is just output label for the object.  */
  ASM_OUTPUT_LABEL (file, name);
#endif /* ASM_DECLARE_OBJECT_NAME */
  ASM_OUTPUT_SKIP (file, size ? size : 1);
}

/* Called via the macro ASM_OUTPUT_DECL_COMMON */
void
c33_output_common (FILE * file,
                    tree decl,
                    const char * name,
                    int size,
                    int align)
{
  if (decl == NULL_TREE)
    {
      fprintf (file, "%s", COMMON_ASM_OP);
    }
  else
    {
      switch (c33_get_data_area (decl))
	{
	case DATA_AREA_ZDA:
	  fprintf (file, "%s", ZCOMMON_ASM_OP);
	  break;

	case DATA_AREA_SDA:
	  fprintf (file, "%s", SCOMMON_ASM_OP);
	  break;

	case DATA_AREA_TDA:
	  fprintf (file, "%s", TCOMMON_ASM_OP);
	  break;

	default:
	  fprintf (file, "%s", COMMON_ASM_OP);
	  break;
	}
    }

  assemble_name (file, name);
  fprintf (file, ",%u,%u\n", size, align / BITS_PER_UNIT);
}

/* Called via the macro ASM_OUTPUT_DECL_LOCAL */
void
c33_output_local (FILE * file,
                   tree decl,
                   const char * name,
                   int size,
                   int align)
{
  fprintf (file, "%s", LOCAL_ASM_OP);
  assemble_name (file, name);
  fprintf (file, "\n");

  ASM_OUTPUT_ALIGNED_DECL_COMMON (file, decl, name, size, align);
}

/* Add data area to the given declaration if a ghs data area pragma is
   currently in effect (#pragma ghs startXXX/endXXX).  */
static void
c33_insert_attributes (tree decl, tree * attr_ptr ATTRIBUTE_UNUSED )
{
  if (data_area_stack
      && data_area_stack->data_area
      && current_function_decl == NULL_TREE
      && (VAR_P (decl) || TREE_CODE (decl) == CONST_DECL)
      && c33_get_data_area (decl) == DATA_AREA_NORMAL)
    c33_set_data_area (decl, data_area_stack->data_area);

  /* Initialize the default names of the c33 specific sections,
     if this has not been done before.  */

  if (GHS_default_section_names [(int) GHS_SECTION_KIND_SDATA] == NULL)
    {
      GHS_default_section_names [(int) GHS_SECTION_KIND_SDATA]
	= ".sdata";

      GHS_default_section_names [(int) GHS_SECTION_KIND_ROSDATA]
	= ".rosdata";

      GHS_default_section_names [(int) GHS_SECTION_KIND_TDATA]
	= ".tdata";

      GHS_default_section_names [(int) GHS_SECTION_KIND_ZDATA]
	= ".zdata";

      GHS_default_section_names [(int) GHS_SECTION_KIND_ROZDATA]
	= ".rozdata";
    }

  if (current_function_decl == NULL_TREE
      && (VAR_P (decl)
	  || TREE_CODE (decl) == CONST_DECL
	  || TREE_CODE (decl) == FUNCTION_DECL)
      && (!DECL_EXTERNAL (decl) || DECL_INITIAL (decl))
      && !DECL_SECTION_NAME (decl))
    {
      enum GHS_section_kind kind = GHS_SECTION_KIND_DEFAULT;
      const char * chosen_section;

      if (TREE_CODE (decl) == FUNCTION_DECL)
	kind = GHS_SECTION_KIND_TEXT;
      else
	{
	  /* First choose a section kind based on the data area of the decl.  */
	  switch (c33_get_data_area (decl))
	    {
	    default:
	      gcc_unreachable ();

	    case DATA_AREA_SDA:
	      kind = ((TREE_READONLY (decl))
		      ? GHS_SECTION_KIND_ROSDATA
		      : GHS_SECTION_KIND_SDATA);
	      break;

	    case DATA_AREA_TDA:
	      kind = GHS_SECTION_KIND_TDATA;
	      break;

	    case DATA_AREA_ZDA:
	      kind = ((TREE_READONLY (decl))
		      ? GHS_SECTION_KIND_ROZDATA
		      : GHS_SECTION_KIND_ZDATA);
	      break;

	    case DATA_AREA_NORMAL:		 /* default data area */
	      if (TREE_READONLY (decl))
		kind = GHS_SECTION_KIND_RODATA;
	      else if (DECL_INITIAL (decl))
		kind = GHS_SECTION_KIND_DATA;
	      else
		kind = GHS_SECTION_KIND_BSS;
	    }
	}

      /* Now, if the section kind has been explicitly renamed,
         then attach a section attribute.  */
      chosen_section = GHS_current_section_names [(int) kind];

      /* Otherwise, if this kind of section needs an explicit section
         attribute, then also attach one.  */
      if (chosen_section == NULL)
        chosen_section = GHS_default_section_names [(int) kind];

      if (chosen_section)
	{
	  /* Only set the section name if specified by a pragma, because
	     otherwise it will force those variables to get allocated storage
	     in this module, rather than by the linker.  */
	  set_decl_section_name (decl, chosen_section);
	}
    }
}

/* Construct a DISPOSE instruction that is the equivalent of
   the given RTX.  We have already verified that this should
   be possible.  */


/* Construct a PREPARE instruction that is the equivalent of
   the given RTL.  We have already verified that this should
   be possible.  */


/* Return an RTX indicating where the return address to the
   calling function can be found.  */

rtx
c33_return_addr (int count)
{
  if (count != 0)
    return const0_rtx;

  /* call pushes the return address onto the stack (core manual 2.4.4), so
     there is no register holding it -- it sits just above the frame.  */
  return gen_rtx_MEM (Pmode, plus_constant (Pmode, arg_pointer_rtx, -4));
}

/* Implement TARGET_ASM_INIT_SECTIONS.  */

static void
c33_asm_init_sections (void)
{
  rosdata_section
    = get_unnamed_section (0, output_section_asm_op,
			   "\t.section .rosdata,\"a\"");

  rozdata_section
    = get_unnamed_section (0, output_section_asm_op,
			   "\t.section .rozdata,\"a\"");

  tdata_section
    = get_unnamed_section (SECTION_WRITE, output_section_asm_op,
			   "\t.section .tdata,\"aw\"");

  zdata_section
    = get_unnamed_section (SECTION_WRITE, output_section_asm_op,
			   "\t.section .zdata,\"aw\"");

  zbss_section
    = get_unnamed_section (SECTION_WRITE | SECTION_BSS,
			   output_section_asm_op,
			   "\t.section .zbss,\"aw\"");
}

static section *
c33_select_section (tree exp,
                     int reloc ATTRIBUTE_UNUSED,
                     unsigned HOST_WIDE_INT align ATTRIBUTE_UNUSED)
{
  if (TREE_CODE (exp) == VAR_DECL)
    {
      int is_const;
      if (!TREE_READONLY (exp)
	  || !DECL_INITIAL (exp)
	  || (DECL_INITIAL (exp) != error_mark_node
	      && !TREE_CONSTANT (DECL_INITIAL (exp))))
        is_const = FALSE;
      else
        is_const = TRUE;

      switch (c33_get_data_area (exp))
        {
        case DATA_AREA_ZDA:
	  return is_const ? rozdata_section : zdata_section;

        case DATA_AREA_TDA:
	  return tdata_section;

        case DATA_AREA_SDA:
	  return is_const ? rosdata_section : sdata_section;

        default:
	  return is_const ? readonly_data_section : data_section;
        }
    }
  return readonly_data_section;
}

/* Worker function for TARGET_FUNCTION_VALUE_REGNO_P.  */

static bool
c33_function_value_regno_p (const unsigned int regno)
{
  return (regno == RV_REGNUM);
}

/* Worker function for TARGET_RETURN_IN_MEMORY.  */

static bool
c33_return_in_memory (const_tree type, const_tree fntype ATTRIBUTE_UNUSED)
{
  /* Return values > 8 bytes in length in memory.  */
  return int_size_in_bytes (type) > 8
    || TYPE_MODE (type) == BLKmode
    /* With the rh850 ABI return all aggregates in memory.  */
    || ((! TARGET_GCC_ABI) && AGGREGATE_TYPE_P (type))
    ;
}

/* Worker function for TARGET_FUNCTION_VALUE.  */

static rtx
c33_function_value (const_tree valtype,
                    const_tree fn_decl_or_type ATTRIBUTE_UNUSED,
                    bool outgoing ATTRIBUTE_UNUSED)
{
  return gen_rtx_REG (TYPE_MODE (valtype), RV_REGNUM);
}

/* Implement TARGET_LIBCALL_VALUE.  */

static rtx
c33_libcall_value (machine_mode mode,
		    const_rtx func ATTRIBUTE_UNUSED)
{
  return gen_rtx_REG (mode, RV_REGNUM);
}


/* Worker function for TARGET_CAN_ELIMINATE.  */

static bool
c33_can_eliminate (const int from ATTRIBUTE_UNUSED, const int to)
{
  return (to == STACK_POINTER_REGNUM ? ! frame_pointer_needed : true);
}

/* Worker function for TARGET_CONDITIONAL_REGISTER_USAGE.

   If TARGET_APP_REGS is not defined then add r2 and r5 to
   the pool of fixed registers. See PR 14505.  */

static void
c33_conditional_register_usage (void)
{
  if (TARGET_APP_REGS)
    {
     fixed_regs[2] = 0;  call_used_regs[2] = 0;
     fixed_regs[5] = 0;  call_used_regs[5] = 1;
    }
}

/* Worker function for TARGET_ASM_TRAMPOLINE_TEMPLATE.  */

static void
c33_asm_trampoline_template (FILE *f)
{
  fprintf (f, "\tjarl .+4,r12\n");
  fprintf (f, "\tld.w 12[r12],r19\n");
  fprintf (f, "\tld.w 16[r12],r12\n");
  fprintf (f, "\tjmp [r12]\n");
  fprintf (f, "\tnop\n");
  fprintf (f, "\t.long 0\n");
  fprintf (f, "\t.long 0\n");
}

/* Worker function for TARGET_TRAMPOLINE_INIT.  */

static void
c33_trampoline_init (rtx m_tramp, tree fndecl, rtx chain_value)
{
  rtx mem, fnaddr = XEXP (DECL_RTL (fndecl), 0);

  emit_block_move (m_tramp, assemble_trampoline_template (),
		   GEN_INT (TRAMPOLINE_SIZE), BLOCK_OP_NORMAL);

  mem = adjust_address (m_tramp, SImode, 16);
  emit_move_insn (mem, chain_value);
  mem = adjust_address (m_tramp, SImode, 20);
  emit_move_insn (mem, fnaddr);
}

static int
c33_issue_rate (void)
{
  return (TARGET_C33E2_UP ? 2 : 1);
}

/* Implement TARGET_LEGITIMATE_CONSTANT_P.  */

static bool
c33_legitimate_constant_p (machine_mode mode ATTRIBUTE_UNUSED, rtx x)
{
  return (GET_CODE (x) == CONST_DOUBLE
	  || !(GET_CODE (x) == CONST
	       && GET_CODE (XEXP (x, 0)) == PLUS
	       && GET_CODE (XEXP (XEXP (x, 0), 0)) == SYMBOL_REF
	       && GET_CODE (XEXP (XEXP (x, 0), 1)) == CONST_INT
	       && !CONST_OK_FOR_K (INTVAL (XEXP (XEXP (x, 0), 1)))));
}

/* Helper function for `c33_legitimate_address_p'.  */

static bool
c33_reg_ok_for_base_p (const_rtx reg, bool strict_p)
{
  if (strict_p)
  {
    return REGNO_OK_FOR_BASE_P (REGNO (reg));
  } else {
    return true;
  }
}

/* Accept either REG or SUBREG where a register is valid.  */

static bool
c33_rtx_ok_for_base_p (const_rtx x, bool strict_p)
{
  return ((REG_P (x) && c33_reg_ok_for_base_p  (x, strict_p))
	  || (SUBREG_P (x) && REG_P (SUBREG_REG (x))
	      && c33_reg_ok_for_base_p (SUBREG_REG (x), strict_p)));
}

/* Implement TARGET_LEGITIMATE_ADDRESS_P.  */

static bool
c33_legitimate_address_p (machine_mode mode, rtx x, bool strict_p,
			   addr_space_t as ATTRIBUTE_UNUSED,
			   code_helper = ERROR_MARK)
{
  gcc_assert (ADDR_SPACE_GENERIC_P (as));

  /* [%rb] and [%sp] -- plain register indirect (core manual 5.5.3).  */
  if (c33_rtx_ok_for_base_p (x, strict_p))
    return true;

  /* [%rb]+ -- register indirect with post-increment (5.5.4).  */
  if (GET_CODE (x) == POST_INC
      && c33_rtx_ok_for_base_p (XEXP (x, 0), strict_p))
    return true;

  /* base + displacement.  Unextended this only exists as [%sp+imm6], where
     imm6 is scaled by the transfer size (5.5.5); with one or two ext
     prefixes any base gets a 13- or 26-bit unsigned displacement (5.6.2).
     Since the assembler synthesises the ext prefixes for us, accept any
     constant that fits 26 bits, and require natural alignment so the
     scaled unextended form stays available.  */
  if (GET_CODE (x) == PLUS
      && c33_rtx_ok_for_base_p (XEXP (x, 0), strict_p)
      && CONST_INT_P (XEXP (x, 1)))
    {
      HOST_WIDE_INT off = INTVAL (XEXP (x, 1));
      unsigned size = GET_MODE_SIZE (mode);

      if (off < 0 || off >= (HOST_WIDE_INT) 1 << 26)
	return false;
      if (size == 2 && (off & 1) != 0)
	return false;
      if (size >= 4 && (off & 3) != 0)
	return false;
      return true;
    }

  /* An absolute address, materialised by the assembler as an xld.w of the
     symbol (the R_C33_H/M/L triple).  */
  if (CONSTANT_ADDRESS_P (x))
    return true;

  return false;
}

static int
c33_memory_move_cost (machine_mode mode,
		       reg_class_t reg_class ATTRIBUTE_UNUSED,
		       bool in)
{
  switch (GET_MODE_SIZE (mode))
    {
    case 0:
      return in ? 24 : 8;
    case 1:
    case 2:
    case 3:
    case 4:
      return in ? 6 : 2;
    default:
      return (GET_MODE_SIZE (mode) / 2) * (in ? 3 : 1);
    }
}

int
c33_adjust_insn_length (rtx_insn *insn, int length)
{
  if (TARGET_C33E3V5_UP)
    {
      if (CALL_P (insn))
	{
	  if (TARGET_LONG_CALLS)
	    {
	      /* call_internal_long, call_value_internal_long.  */
	      if (length == 8)
		length = 4;
	      if (length == 16)
		length = 10;
	    }
	  else
	    {
	      /* call_internal_short, call_value_internal_short.  */
	      if (length == 8)
		length = 4;
	    }
	}
    }
  return length;
}

/* C33 specific attributes.  */

TARGET_GNU_ATTRIBUTES (c33_attribute_table,
{
  /* { name, min_len, max_len, decl_req, type_req, fn_type_req,
       affects_type_identity, handler, exclude } */
  { "interrupt_handler", 0, 0, true,  false, false, false,
    c33_handle_interrupt_attribute, NULL },
  { "interrupt",         0, 0, true,  false, false, false,
    c33_handle_interrupt_attribute, NULL },
  { "sda",               0, 0, true,  false, false, false,
    c33_handle_data_area_attribute, NULL },
  { "tda",               0, 0, true,  false, false, false,
    c33_handle_data_area_attribute, NULL },
  { "zda",               0, 0, true,  false, false, false,
    c33_handle_data_area_attribute, NULL }
});

static void
c33_option_override (void)
{
  if (flag_exceptions || flag_non_call_exceptions)
    flag_omit_frame_pointer = 0;

  /* The RH850 ABI does not (currently) support the use of the CALLT instruction.  */
  if (! TARGET_GCC_ABI)
    target_flags |= MASK_DISABLE_CALLT;

  /* Save the initial options in case the user does function specific
     options.  */
  target_option_default_node = target_option_current_node
    = build_target_option_node (&global_options, &global_options_set);
}

const char *
c33_gen_movdi (rtx * operands)
{
  if (REG_P (operands[0]))
    {
      if (REG_P (operands[1]))
	{
	  if (REGNO (operands[0]) == (REGNO (operands[1]) - 1))
	    return "mov %1, %0; mov %R1, %R0";

	  return "mov %R1, %R0; mov %1, %0";
	}

      if (MEM_P (operands[1]))
	{
	  if (REGNO (operands[0]) & 1)
	    /* Use two load word instructions to synthesise a load double.  */
	    return "ld.w %1, %0 ; ld.w %R1, %R0" ;

	  return "ld.dw %1, %0";
	}

      return "mov %1, %0; mov %R1, %R0";
    }

  gcc_assert (REG_P (operands[1]));

  if (REGNO (operands[1]) & 1)
    /* Use two store word instructions to synthesise a store double.  */
    return "st.w %1, %0 ; st.w %R1, %R0 ";

  return "st.dw %1, %0";
}

/* Implement TARGET_HARD_REGNO_MODE_OK.  */

static bool
c33_hard_regno_mode_ok (unsigned int regno, machine_mode mode)
{
  return GET_MODE_SIZE (mode) <= 4 || ((regno & 1) == 0 && regno != 0);
}

/* Implement TARGET_MODES_TIEABLE_P.  */

static bool
c33_modes_tieable_p (machine_mode mode1, machine_mode mode2)
{
  return (mode1 == mode2
	  || (GET_MODE_SIZE (mode1) <= 4 && GET_MODE_SIZE (mode2) <= 4));
}

static bool
c33_can_inline_p (tree caller, tree callee)
{
  tree caller_tree = DECL_FUNCTION_SPECIFIC_TARGET (caller);
  tree callee_tree = DECL_FUNCTION_SPECIFIC_TARGET (callee);

  const unsigned HOST_WIDE_INT safe_flags = MASK_PROLOG_FUNCTION;

  if (!callee_tree)
    callee_tree = target_option_default_node;
  if (!caller_tree)
    caller_tree = target_option_default_node;
  if (callee_tree == caller_tree)
    return true;

  cl_target_option *caller_opts = TREE_TARGET_OPTION (caller_tree);
  cl_target_option *callee_opts = TREE_TARGET_OPTION (callee_tree);

  return ((caller_opts->x_target_flags & ~safe_flags)
	  == (callee_opts->x_target_flags & ~safe_flags));
}


/* Initialize the GCC target structure.  */

#undef  TARGET_OPTION_OVERRIDE
#define TARGET_OPTION_OVERRIDE		c33_option_override

#undef  TARGET_MEMORY_MOVE_COST
#define TARGET_MEMORY_MOVE_COST 	c33_memory_move_cost

#undef  TARGET_ASM_ALIGNED_HI_OP
#define TARGET_ASM_ALIGNED_HI_OP "\t.hword\t"

#undef  TARGET_PRINT_OPERAND
#define TARGET_PRINT_OPERAND 		c33_print_operand
#undef  TARGET_PRINT_OPERAND_ADDRESS
#define TARGET_PRINT_OPERAND_ADDRESS 		c33_print_operand_address
#undef  TARGET_PRINT_OPERAND_PUNCT_VALID_P
#define TARGET_PRINT_OPERAND_PUNCT_VALID_P 	c33_print_operand_punct_valid_p

#undef TARGET_ASM_OUTPUT_ADDR_CONST_EXTRA
#define TARGET_ASM_OUTPUT_ADDR_CONST_EXTRA c33_output_addr_const_extra

#undef  TARGET_ATTRIBUTE_TABLE
#define TARGET_ATTRIBUTE_TABLE c33_attribute_table

#undef  TARGET_INSERT_ATTRIBUTES
#define TARGET_INSERT_ATTRIBUTES c33_insert_attributes

#undef  TARGET_ASM_SELECT_SECTION
#define TARGET_ASM_SELECT_SECTION  c33_select_section

/* The assembler supports switchable .bss sections, but
   c33_select_section doesn't yet make use of them.  */
#undef  TARGET_HAVE_SWITCHABLE_BSS_SECTIONS
#define TARGET_HAVE_SWITCHABLE_BSS_SECTIONS false

#undef  TARGET_ENCODE_SECTION_INFO
#define TARGET_ENCODE_SECTION_INFO c33_encode_section_info

#undef  TARGET_ASM_FILE_START_FILE_DIRECTIVE
#define TARGET_ASM_FILE_START_FILE_DIRECTIVE true

#undef  TARGET_RTX_COSTS
#define TARGET_RTX_COSTS c33_rtx_costs

#undef  TARGET_ADDRESS_COST
#define TARGET_ADDRESS_COST hook_int_rtx_mode_as_bool_0

#undef  TARGET_MACHINE_DEPENDENT_REORG
#define TARGET_MACHINE_DEPENDENT_REORG c33_reorg

#undef  TARGET_SCHED_ISSUE_RATE
#define TARGET_SCHED_ISSUE_RATE c33_issue_rate

#undef  TARGET_FUNCTION_VALUE_REGNO_P
#define TARGET_FUNCTION_VALUE_REGNO_P c33_function_value_regno_p
#undef  TARGET_FUNCTION_VALUE
#define TARGET_FUNCTION_VALUE c33_function_value
#undef  TARGET_LIBCALL_VALUE
#define TARGET_LIBCALL_VALUE c33_libcall_value

#undef  TARGET_PROMOTE_PROTOTYPES
#define TARGET_PROMOTE_PROTOTYPES hook_bool_const_tree_true

#undef  TARGET_RETURN_IN_MEMORY
#define TARGET_RETURN_IN_MEMORY c33_return_in_memory

#undef  TARGET_PASS_BY_REFERENCE
#define TARGET_PASS_BY_REFERENCE c33_pass_by_reference

#undef  TARGET_CALLEE_COPIES
#define TARGET_CALLEE_COPIES hook_bool_CUMULATIVE_ARGS_arg_info_true

#undef  TARGET_ARG_PARTIAL_BYTES
#define TARGET_ARG_PARTIAL_BYTES c33_arg_partial_bytes

#undef  TARGET_FUNCTION_ARG
#define TARGET_FUNCTION_ARG c33_function_arg

#undef  TARGET_FUNCTION_ARG_ADVANCE
#define TARGET_FUNCTION_ARG_ADVANCE c33_function_arg_advance

#undef  TARGET_CAN_ELIMINATE
#define TARGET_CAN_ELIMINATE c33_can_eliminate

#undef  TARGET_CONDITIONAL_REGISTER_USAGE
#define TARGET_CONDITIONAL_REGISTER_USAGE c33_conditional_register_usage

#undef  TARGET_ASM_TRAMPOLINE_TEMPLATE
#define TARGET_ASM_TRAMPOLINE_TEMPLATE c33_asm_trampoline_template
#undef  TARGET_TRAMPOLINE_INIT
#define TARGET_TRAMPOLINE_INIT c33_trampoline_init

#undef  TARGET_LEGITIMATE_CONSTANT_P
#define TARGET_LEGITIMATE_CONSTANT_P c33_legitimate_constant_p

#undef  TARGET_ADDR_SPACE_LEGITIMATE_ADDRESS_P
#define TARGET_ADDR_SPACE_LEGITIMATE_ADDRESS_P c33_legitimate_address_p

#undef  TARGET_CAN_USE_DOLOOP_P
#define TARGET_CAN_USE_DOLOOP_P can_use_doloop_if_innermost

#undef  TARGET_HARD_REGNO_MODE_OK
#define TARGET_HARD_REGNO_MODE_OK c33_hard_regno_mode_ok

#undef  TARGET_MODES_TIEABLE_P
#define TARGET_MODES_TIEABLE_P c33_modes_tieable_p

#undef TARGET_FLAGS_REGNUM
#define TARGET_FLAGS_REGNUM 32

#undef  TARGET_HAVE_SPECULATION_SAFE_VALUE
#define TARGET_HAVE_SPECULATION_SAFE_VALUE speculation_safe_value_not_needed

#undef TARGET_CAN_INLINE_P
#define TARGET_CAN_INLINE_P c33_can_inline_p

#undef TARGET_DOCUMENTATION_NAME
#define TARGET_DOCUMENTATION_NAME "C33"

struct gcc_target targetm = TARGET_INITIALIZER;

#include "gt-c33.h"
