/* Definitions of target machine for GNU compiler. NEC C33 series
   Copyright (C) 1996-2026 Free Software Foundation, Inc.
   Contributed by Jeff Law (law@cygnus.com).

   This file is part of GCC.

   GCC is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3, or (at your option)
   any later version.

   GCC is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   Under Section 7 of GPL version 3, you are granted additional
   permissions described in the GCC Runtime Library Exception, version
   3.1, as published by the Free Software Foundation.

   You should have received a copy of the GNU General Public License and
   a copy of the GCC Runtime Library Exception along with this program;
   see the files COPYING3 and COPYING.RUNTIME respectively.  If not, see
   <http://www.gnu.org/licenses/>.  */

#ifndef GCC_C33_H
#define GCC_C33_H

#undef LIB_SPEC
#define LIB_SPEC "%{!shared:%{!symbolic:--start-group -lc -lgcc --end-group}}"

#undef ENDFILE_SPEC
#undef LINK_SPEC
#undef STARTFILE_SPEC
#undef ASM_SPEC

/* Flags the V850 option set had that the C33 does not.  Rather than thread
   the removals through every use, they are pinned here to the value that is
   true for this target, with the reason.  Simplifying the code that reads
   them is cleanup still owed -- see "Next steps" in gcc/README.md.  */

/* Structures larger than 8 bytes are passed in memory, with the caller
   supplying a hidden pointer to the return slot as the first argument
   (ABI.md, derived from the original compiler).  That is what V850 called
   its "old GCC ABI"; the RH850 alternative is not this target's.  */
#define TARGET_GCC_ABI		1

/* No ep register: V850 reserved r30 as a short-addressing base.  The C33's
   analogue is %r15 for the default data area, handled separately.  */
#define TARGET_EP		0

/* All of %r0-%r15 are ordinary general registers here; V850 reserved r2 and
   r5 for the application.  */
#define TARGET_APP_REGS		1

/* sld/sst and callt are V850 instructions.  */
#define TARGET_SMALL_SLD	0
#define TARGET_DISABLE_CALLT	1

/* No FPU on any C33 core; floating point is soft, through libgcc.  */
#define TARGET_SOFT_FLOAT	1
#define TARGET_USE_FPU		0

/* e3v5 hardware loop, and its 64-bit alignment option.  */
#define TARGET_LOOP		0
#define TARGET_8BYTE_ALIGN	0

/* NEC core variants.  Nothing gates on these any more; they are here only
   so the few remaining cost calculations still compile.  */
#define TARGET_C33E		0
#define TARGET_C33E_UP		0
#define TARGET_C33E2_UP		0
#define TARGET_C33E2V3_UP	0
#define TARGET_C33E3V5_UP	0

/* The C33 core variant, selected by -mc33 / -mc33adv / -mc33pe (or -mcore=).
   The WikiReader is a PE.  This replaces the V850 e/e1/e2/e2v3/e3v5 ladder
   the port was forked from -- those were NEC cores, not Epson ones, and
   nothing in this backend gates on them any more.  */

#define TARGET_C33_STD  (c33_selected_core == C33_CORE_STD)
#define TARGET_C33_ADV  (c33_selected_core == C33_CORE_ADV)
#define TARGET_C33_PE   (c33_selected_core == C33_CORE_PE)

/* Pass the core through to the assembler, which needs it to pick the right
   opcode table -- the ADV and PE cores have instructions the base core does
   not.  There is no -mc33: the base core is the assembler's default, and
   passing it is rejected as an ambiguous prefix of -mc33adv/-mc33pe.

   Both spellings are matched because -mc33pe is an Alias of -mcore=.  */
#define ASM_SPEC \
  "%{mc33adv|mcore=c33adv:-mc33adv} %{mc33pe|mcore=c33pe:-mc33pe}"

#define LINK_SPEC ""
#define CPP_SPEC ""

#define TARGET_CPU_CPP_BUILTINS()		\
  do						\
    {						\
      builtin_define ("__c33");			\
      builtin_define ("__c33__");		\
      builtin_assert ("machine=c33");		\
      builtin_assert ("cpu=c33");		\
      if (TARGET_C33_ADV)			\
	builtin_define ("__c33adv__");		\
      if (TARGET_C33_PE)			\
	builtin_define ("__c33pe__");		\
      if (TARGET_EXT_32)			\
	builtin_define ("__C33_EDDA32__");	\
      /* No FPU on any C33: floating point is soft, through libgcc.  */ \
      builtin_define ("__NO_FPU__");		\
    }						\
  while (0)



/* Target machine storage layout */

/* Define this if most significant bit is lowest numbered
   in instructions that operate on numbered bit-fields.
   This is not true on the NEC C33.  */
#define BITS_BIG_ENDIAN 0

/* Define this if most significant byte of a word is the lowest numbered.  */
/* This is not true on the NEC C33.  */
#define BYTES_BIG_ENDIAN 0

/* Define this if most significant word of a multiword number is lowest
   numbered.
   This is not true on the NEC C33.  */
#define WORDS_BIG_ENDIAN 0

/* Width of a word, in units (bytes).  */
#define UNITS_PER_WORD		4

/* Define this macro if it is advisable to hold scalars in registers
   in a wider mode than that declared by the program.  In such cases,
   the value is constrained to be within the bounds of the declared
   type, but kept valid in the wider mode.  The signedness of the
   extension may differ from that of the type.

   Some simple experiments have shown that leaving UNSIGNEDP alone
   generates the best overall code.  */

#define PROMOTE_MODE(MODE,UNSIGNEDP,TYPE)  \
  if (GET_MODE_CLASS (MODE) == MODE_INT \
      && GET_MODE_SIZE (MODE) < 4)      \
    { (MODE) = SImode; }

/* Allocation boundary (in *bits*) for storing arguments in argument list.  */
#define PARM_BOUNDARY		32

/* The stack goes in 32-bit lumps.  */
#define STACK_BOUNDARY 		BIGGEST_ALIGNMENT

/* Allocation boundary (in *bits*) for the code of a function.
   16 is the minimum boundary; 32 would give better performance.  */
/* Instructions are 2-byte aligned (core manual 5.1).  */
#define FUNCTION_BOUNDARY 	16

/* No data type wants to be aligned rounder than this.  */
#define BIGGEST_ALIGNMENT	32

/* Alignment of field after `int : 0' in a structure.  */
#define EMPTY_FIELD_BOUNDARY 32

/* No structure field wants to be aligned rounder than this.  */
#define BIGGEST_FIELD_ALIGNMENT BIGGEST_ALIGNMENT

/* Define this if move instructions will actually fail to work
   when given unaligned data.  */
#define STRICT_ALIGNMENT  (!TARGET_NO_STRICT_ALIGN)

/* Define this as 1 if `char' should by default be signed; else as 0.

   On the NEC C33, loads do sign extension, so make this default.  */
#define DEFAULT_SIGNED_CHAR 1

#undef  SIZE_TYPE
#define SIZE_TYPE "unsigned int"

#undef  PTRDIFF_TYPE
#define PTRDIFF_TYPE "int"

#undef  WCHAR_TYPE
#define WCHAR_TYPE "long int"

#undef  WCHAR_TYPE_SIZE
#define WCHAR_TYPE_SIZE BITS_PER_WORD

/* Standard register usage.  */

/* Number of actual hardware registers.
   The hardware registers are assigned numbers for the compiler
   from 0 to just below FIRST_PSEUDO_REGISTER.

   All registers that the compiler knows about must be given numbers,
   even those that are not normally considered general registers.  */

/* The C33 has sixteen general registers %r0-%r15 (core manual table 2.9.1.1)
   plus the special registers reachable with ld.w %rd,%ss / ld.w %sd,%rs
   (table 2.9.2.1).  We model the ones the compiler needs:

     0-15  %r0-%r15   general registers
       16  %sp        stack pointer
       17  %alr       multiply result, low 32 bits
       18  %ahr       multiply result, high 32 bits
       19  CC         PSR condition flags (N, Z, V, C), for MODE_CC
       20  .fp        virtual frame pointer, always eliminated
       21  .ap        virtual arg pointer, always eliminated

   Register roles, from the ABI (see host-tools/toolchain-c33/gcc/ABI.md):

     %r0-%r3    callee-saved
     %r4-%r5    return value (%r4 low, %r5 high)
     %r6-%r9    first four arguments; %r9 doubles as the static chain
     %r10-%r14  call-clobbered scratch
     %r15       default data area pointer (__dp)  */

#define FIRST_PSEUDO_REGISTER 22

/* 1 for registers that have pervasive standard uses
   and are not available for the register allocator.

   Only %r15 is reserved among the general registers: it is the base for
   %r15-relative addressing of the default data area.  Under -medda32 nothing
   uses it, and TARGET_CONDITIONAL_REGISTER_USAGE frees it.

   Note %r10-%r14 are allocatable here where the 3.3.2 backend fixed them.
   They are call-clobbered in the original ABI and the hand-written assembly
   in samo-lib only ever uses them as scratch inside a routine, so allocating
   them changes no interface -- it just gives us 15 usable registers
   instead of 10.  */

#define FIXED_REGISTERS \
  { 0, 0, 0, 0, 0, 0, 0, 0, \
    0, 0, 0, 0, 0, 0, 0, 1, \
    1, 1, 1, 1, 1, 1 }

/* 1 for registers not available across function calls.
   These must include the FIXED_REGISTERS and also any
   registers that can be used without being saved.
   The latter must include the registers where values are returned
   and the register where structure-value addresses are passed.
   Aside from that, you can include as many other registers as you
   like.

   %r0-%r3 are the only callee-saved registers; everything else is
   clobbered by a call.  */

#define CALL_USED_REGISTERS \
  { 0, 0, 0, 0, 1, 1, 1, 1, \
    1, 1, 1, 1, 1, 1, 1, 1, \
    1, 1, 1, 1, 1, 1 }

/* List the order in which to allocate registers.  Each register must be
   listed once, even those in FIXED_REGISTERS.

   On the 850, we make the return registers first, then all of the volatile
   registers, then the saved registers in reverse order to better save the
   registers with an out of line function, and finally the fixed
   registers.  */

/* Prefer the call-clobbered registers, so leaf functions need no prologue,
   and only then fall back on the callee-saved ones.  */

#define REG_ALLOC_ORDER							\
{									\
   4,  5,				/* return value */		\
   6,  7,  8,  9,			/* argument registers */	\
  10, 11, 12, 13, 14,			/* call-clobbered scratch */	\
   3,  2,  1,  0,			/* callee-saved */		\
  15,					/* %r15, the data pointer */	\
  16, 17, 18, 19, 20, 21		/* sp, alr, ahr, cc, fp, ap */	\
}


/* Define the classes of registers for register constraints in the
   machine description.  Also define ranges of constants.

   One of the classes must always be named ALL_REGS and include all hard regs.
   If there is more than one class, another class must be named NO_REGS
   and contain no registers.

   The name GENERAL_REGS must be the name of a class (or an alias for
   another name such as ALL_REGS).  This is the class of registers
   that is allowed by "g" or "r" in a register constraint.
   Also, registers outside this class are allocated only when
   instructions express preferences for them.

   The classes must be numbered in nondecreasing order; that is,
   a larger-numbered class must never be contained completely
   in a smaller-numbered class.

   For any two classes, it is very desirable that there be another
   class that represents their union.  */

enum reg_class
{
  NO_REGS, EVEN_REGS, GENERAL_REGS, SP_REGS, BASE_REGS, ALL_REGS,
  LIM_REG_CLASSES
};

#define N_REG_CLASSES (int) LIM_REG_CLASSES

/* Give names of register classes as strings for dump file.  */

#define REG_CLASS_NAMES							\
{ "NO_REGS", "EVEN_REGS", "GENERAL_REGS", "SP_REGS", "BASE_REGS",	\
  "ALL_REGS", "LIM_REGS" }

/* Define which registers fit in which classes.
   This is an initializer for a vector of HARD_REG_SET
   of length N_REG_CLASSES.  */

/* The C33 has no even-alignment requirement on 64-bit register pairs -- a
   DImode value lives in %rN/%rN+1, low word first, for any N.  So EVEN_REGS,
   which the V850 used for its pair-alignment rule, is just GENERAL_REGS here.

   Keeping it a strict subset is actively harmful: LRA will narrow a reload to
   the smaller class and then be unable to copy the arg pointer (a member of
   GENERAL_REGS only) into it, and loop until it hits the reload limit.

   %sp is architecturally a *system* register on this target, not one of the
   sixteen general registers, so it gets a class of its own and BASE_REGS is
   the union.  This is not cosmetic.  BASE_REG_CLASS must contain %sp, because
   LRA tests whether an eliminable register is a valid base by folding it to
   its elimination target and asking for class membership -- see
   lra_eliminate_reg_if_possible, which substitutes ep->to_rtx and drops the
   offset.  With %sp outside the base class, [.ap + N] is judged an invalid
   address, LRA reloads the base into a pseudo, the reload itself contains .ap
   and is judged invalid the same way, and it recurses until it hits the
   reload limit.  The 3.3.2 backend had exactly these classes.  */

#define REG_CLASS_CONTENTS                     \
{                                              \
  { 0x00000000 }, /* NO_REGS       */          \
  { 0x0030ffff }, /* EVEN_REGS   = GENERAL_REGS */ \
  { 0x0030ffff }, /* GENERAL_REGS: %r0-%r15 + .fp/.ap */ \
  { 0x00010000 }, /* SP_REGS:      %sp */      \
  { 0x0031ffff }, /* BASE_REGS:    GENERAL_REGS + %sp */ \
  { 0x003fffff }, /* ALL_REGS      */          \
}

/* The same information, inverted:
   Return the class number of the smallest class containing
   reg number REGNO.  This could be a conditional expression
   or could index an array.  */

/* The virtual frame and arg pointers must report GENERAL_REGS: they appear
   in ordinary insns until reload eliminates them, so the patterns have to
   accept them.  Only %alr, %ahr and CC are genuinely unallocatable.  */
#define REGNO_REG_CLASS(REGNO)						\
  ((REGNO) == STACK_POINTER_REGNUM ? SP_REGS				\
   : ((REGNO) < 16 || (REGNO) == FRAME_POINTER_REGNUM			\
      || (REGNO) == ARG_POINTER_REGNUM) ? GENERAL_REGS : NO_REGS)

/* The class value for index registers, and the one for base regs.  */

#define INDEX_REG_CLASS NO_REGS
#define BASE_REG_CLASS  BASE_REGS

/* Macros to check register numbers against specific register classes.  */

/* These assume that REGNO is a hard or pseudo reg number.
   They give nonzero only if REGNO is a hard reg of the suitable class
   or a pseudo reg currently allocated to a suitable hard reg.
   Since they use reg_renumber, they are safe only once reg_renumber
   has been allocated, which happens in reginfo.cc during register
   allocation.  */

/* %r0-%r15 can be a memory base via [%rb], and %sp via [%sp+imm6] (core
   manual 5.5.3/5.5.5).  The frame and arg pointers are eliminated into one
   of those, so they must be accepted too.  alr/ahr/cc cannot be bases.  */

#define REGNO_OK_FOR_BASE_P(regno)					\
  ((regno) < 16								\
   || (regno) == STACK_POINTER_REGNUM					\
   || (regno) == FRAME_POINTER_REGNUM					\
   || (regno) == ARG_POINTER_REGNUM					\
   || (unsigned) reg_renumber[regno] < 16				\
   || reg_renumber[regno] == STACK_POINTER_REGNUM)

#define REGNO_OK_FOR_INDEX_P(regno) 0

/* Convenience wrappers around insn_const_int_ok_for_constraint.  */

#define CONST_OK_FOR_I(VALUE) \
  insn_const_int_ok_for_constraint (VALUE, CONSTRAINT_I)
#define CONST_OK_FOR_J(VALUE) \
  insn_const_int_ok_for_constraint (VALUE, CONSTRAINT_J)
#define CONST_OK_FOR_K(VALUE) \
  insn_const_int_ok_for_constraint (VALUE, CONSTRAINT_K)
#define CONST_OK_FOR_L(VALUE) \
  insn_const_int_ok_for_constraint (VALUE, CONSTRAINT_L)
#define CONST_OK_FOR_M(VALUE) \
  insn_const_int_ok_for_constraint (VALUE, CONSTRAINT_M)
#define CONST_OK_FOR_N(VALUE) \
  insn_const_int_ok_for_constraint (VALUE, CONSTRAINT_N)
#define CONST_OK_FOR_O(VALUE) \
  insn_const_int_ok_for_constraint (VALUE, CONSTRAINT_O)
#define CONST_OK_FOR_W(VALUE) \
  insn_const_int_ok_for_constraint (VALUE, CONSTRAINT_W)

/* Stack layout; function entry, exit and calling.  */

/* Define this if pushing a word on the stack
   makes the stack pointer a smaller address.  */

#define STACK_GROWS_DOWNWARD 1

/* Define this to nonzero if the nominal address of the stack frame
   is at the high-address end of the local variables;
   that is, each additional local variable allocated
   goes at a more negative offset in the frame.  */

#define FRAME_GROWS_DOWNWARD 1

/* Offset of first parameter from the argument pointer register value.  */
/* Is equal to the size of the saved fp + pc, even if an fp isn't
   saved since the value is used before we know.  */

#define FIRST_PARM_OFFSET(FNDECL) 0

/* Specify the registers used for certain standard purposes.
   The values of these macros are register numbers.  */

/* Register to use for pushing function arguments.  */
#define STACK_POINTER_REGNUM 16

/* Base register for access to local variables of the function.  */
#define FRAME_POINTER_REGNUM 20


/* On some machines the offset between the frame pointer and starting
   offset of the automatic variables is not known until after register
   allocation has been done (for example, because the saved registers
   are between these two locations).  On those machines, define
   `FRAME_POINTER_REGNUM' the number of a special, fixed register to
   be used internally until the offset is known, and define
   `HARD_FRAME_POINTER_REGNUM' to be actual the hard register number
   used for the frame pointer.

   You should define this macro only in the very rare circumstances
   when it is not possible to calculate the offset between the frame
   pointer and the automatic variables until after register
   allocation has been completed.  When this macro is defined, you
   must also indicate in your definition of `ELIMINABLE_REGS' how to
   eliminate `FRAME_POINTER_REGNUM' into either
   `HARD_FRAME_POINTER_REGNUM' or `STACK_POINTER_REGNUM'.

   Do not define this macro if it would be the same as
   `FRAME_POINTER_REGNUM'.  */
#undef  HARD_FRAME_POINTER_REGNUM
#define HARD_FRAME_POINTER_REGNUM 3

/* Base register for access to arguments of the function.  */
#define ARG_POINTER_REGNUM 21

/* Register in which static-chain is passed to a function.
   This must be a call used register.  */
#define STATIC_CHAIN_REGNUM 9

/* If defined, this macro specifies a table of register pairs used to
   eliminate unneeded registers that point into the stack frame.  If
   it is not defined, the only elimination attempted by the compiler
   is to replace references to the frame pointer with references to
   the stack pointer.

   The definition of this macro is a list of structure
   initializations, each of which specifies an original and
   replacement register.

   On some machines, the position of the argument pointer is not
   known until the compilation is completed.  In such a case, a
   separate hard register must be used for the argument pointer.
   This register can be eliminated by replacing it with either the
   frame pointer or the argument pointer, depending on whether or not
   the frame pointer has been eliminated.

   In this case, you might specify:
        #define ELIMINABLE_REGS  \
        {{ARG_POINTER_REGNUM, STACK_POINTER_REGNUM}, \
         {ARG_POINTER_REGNUM, FRAME_POINTER_REGNUM}, \
         {FRAME_POINTER_REGNUM, STACK_POINTER_REGNUM}}

   Note that the elimination of the argument pointer with the stack
   pointer is specified first since that is the preferred elimination.  */

#define ELIMINABLE_REGS							\
{{ FRAME_POINTER_REGNUM, STACK_POINTER_REGNUM },			\
 { FRAME_POINTER_REGNUM, HARD_FRAME_POINTER_REGNUM },			\
 { ARG_POINTER_REGNUM,	 STACK_POINTER_REGNUM },			\
 { ARG_POINTER_REGNUM,   HARD_FRAME_POINTER_REGNUM }}			\

/* This macro returns the initial difference between the specified pair
   of registers.  */

/* The stack on entry looks like

       [sp_entry + 4 + 4n]   incoming stack argument n
       [sp_entry + 4]        first incoming stack argument
       [sp_entry]            return address, pushed by call (manual 2.4.4)

   and the prologue then pushes the callee-saved block and carves out the
   local frame.  The arg pointer denotes the first stack argument, so its
   distance to %sp includes that extra return-address word -- a word the
   V850, which keeps the return address in r31, does not have.  */

#define INITIAL_ELIMINATION_OFFSET(FROM, TO, OFFSET)			\
{									\
  if ((FROM) == FRAME_POINTER_REGNUM)					\
    (OFFSET) = get_frame_size () + crtl->outgoing_args_size;		\
  else if ((FROM) == ARG_POINTER_REGNUM)				\
    (OFFSET) = compute_frame_size (get_frame_size (), (long *) 0)	\
	       + UNITS_PER_WORD;					\
  else									\
    gcc_unreachable ();							\
}

/* Keep the stack pointer constant throughout the function.  */
#define ACCUMULATE_OUTGOING_ARGS 1

#define RETURN_ADDR_RTX(COUNT, FP) c33_return_addr (COUNT)

/* Define a data type for recording info about an argument list
   during the scan of that argument list.  This data type should
   hold all necessary information about the function itself
   and about the args processed so far, enough to enable macros
   such as FUNCTION_ARG to determine where the next arg should go.  */

#define CUMULATIVE_ARGS struct cum_arg
struct cum_arg { int nbytes; };

/* Initialize a variable CUM of type CUMULATIVE_ARGS
   for a call to a function whose data type is FNTYPE.
   For a library call, FNTYPE is 0.  */

#define INIT_CUMULATIVE_ARGS(CUM, FNTYPE, LIBNAME, INDIRECT, N_NAMED_ARGS) \
  do { (CUM).nbytes = 0; } while (0)

/* When a parameter is passed in a register, stack space is still
   allocated for it.  */
#define REG_PARM_STACK_SPACE(DECL) 0

/* 1 if N is a possible register number for function argument passing.

   %r6-%r9 are the four documented argument registers, but an argument that
   starts in the last slot is not split -- c33_function_arg hands back a
   register of the argument's full mode, which can run off the end of that
   set.  An 8-byte scalar starting at %r9 occupies %r9:%r10; a 16-byte
   _Complex long double starting at %r9 reaches %r12.  gcc 3.3.2 does the
   same and its caller and callee agree, so it is the ABI.

   This macro has to describe that, not the documented set.  df marks every
   register satisfying it as defined on entry to the function, and that is
   what tells the later passes an incoming argument lives there.  With the
   range stopping at %r9, -frename-registers helped itself to %r10 as a
   scratch in a function whose second _Complex float argument was still
   sitting in %r9:%r10 -- see complex-7, which aborts at -O3 -funroll-loops
   (which implies -frename-registers) and passes without it.  */

#define FUNCTION_ARG_REGNO_P(N) ((N) >= 6 && (N) <= 12)

#define DEFAULT_PCC_STRUCT_RETURN 0

/* EXIT_IGNORE_STACK should be nonzero if, when returning from a function,
   the stack pointer does not matter.  The value is tested only in
   functions that have frame pointers.
   No definition is equivalent to always zero.  */

#define EXIT_IGNORE_STACK 1

/* Define this macro as a C expression that is nonzero for registers
   used by the epilogue or the `return' pattern.  */

/* The C33 has no link register: ret pops the return address that call
   pushed (core manual 2.4.4), so the epilogue uses no extra register.  */

#define EPILOGUE_USES(REGNO) 0

/* Output assembler code to FILE to increment profiler label # LABELNO
   for profiling a function entry.  */

#define FUNCTION_PROFILER(FILE, LABELNO) ;

/* Length in units of the trampoline for entering a nested function.  */

#define TRAMPOLINE_SIZE 24

/* Addressing modes, and classification of registers for them.  */


/* 1 if X is an rtx for a constant that is a valid address.  */

/* ??? This seems too exclusive.  May get better code by accepting more
   possibilities here, in particular, should accept ZDA_NAME SYMBOL_REFs.  */

#define CONSTANT_ADDRESS_P(X) constraint_satisfied_p (X, CONSTRAINT_K)

/* Maximum number of registers that can appear in a valid memory address.  */

#define MAX_REGS_PER_ADDRESS 1

/* The core has "ld.<sz> %rd,[%rb]+" and "ld.<sz> [%rb]+,%rs" -- register
   indirect with a post-increment of the transfer size (core manual 5.5.4),
   which is exactly what GCC means by post-increment.  Saying so is what
   turns the auto-inc-dec pass on: TARGET_LEGITIMATE_ADDRESS_P accepting
   POST_INC and output_move_single knowing how to print it are not enough
   on their own, because nothing would ever build the address.

   Leaving this out cost a fifth of every byte-filling loop.  mini-libc's
   memset is the whole of an article load on this device, and its inner
   loop came out as store / add 4 / sub / cmp / branch where gcc 3.3.2
   emitted store-and-increment / sub / cmp / branch.  */
#define HAVE_POST_INCREMENT 1

/* Given a comparison code (EQ, NE, etc.) and the first operand of a COMPARE,
   return the mode to be used for the comparison.

   For floating-point equality comparisons, CCFPEQmode should be used.
   VOIDmode should be used in all other cases.

   For integer comparisons against zero, reduce to CCNOmode or CCZmode if
   possible, to allow for more combinations.  */

#define SELECT_CC_MODE(OP, X, Y)       c33_select_cc_mode (OP, X, Y)

/* Nonzero if access to memory by bytes or half words is no faster
   than accessing full words.  */
#define SLOW_BYTE_ACCESS 1

/* According expr.cc, a value of around 6 should minimize code size, and
   for the C33 series, that's our primary concern.  */
#define MOVE_RATIO(speed) 6

/* Indirect calls are expensive, never turn a direct call
   into an indirect call.  */
#define NO_FUNCTION_CSE 1

/* The four different data regions on the c33.  */
typedef enum
{
  DATA_AREA_NORMAL,
  DATA_AREA_SDA,
  DATA_AREA_TDA,
  DATA_AREA_ZDA
} c33_data_area;

#define TEXT_SECTION_ASM_OP  "\t.section .text"
#define DATA_SECTION_ASM_OP  "\t.section .data"
#define BSS_SECTION_ASM_OP   "\t.section .bss"
#define SDATA_SECTION_ASM_OP "\t.section .sdata,\"aw\""
#define SBSS_SECTION_ASM_OP  "\t.section .sbss,\"aw\""

#define SCOMMON_ASM_OP 	       "\t.scomm\t"
#define ZCOMMON_ASM_OP 	       "\t.zcomm\t"
#define TCOMMON_ASM_OP 	       "\t.tcomm\t"

/* The C33 assembler comments with ";", not "#".  */
#define ASM_COMMENT_START ";"

/* Output to assembler file text saying following lines
   may contain character constants, extra white space, comments, etc.  */

#define ASM_APP_ON ";APP\n"

/* Output to assembler file text saying following lines
   no longer contain unusual constructs.  */

#define ASM_APP_OFF ";NO_APP\n"

/* The EPSON toolchain emits C symbols unadorned -- samo-lib's hand-written
   assembly declares e.g. ".global exit", not "_exit".  */
#undef  USER_LABEL_PREFIX
#define USER_LABEL_PREFIX ""

/* This says how to output the assembler to define a global
   uninitialized but not common symbol.  */

#define ASM_OUTPUT_ALIGNED_BSS(FILE, DECL, NAME, SIZE, ALIGN) \
  asm_output_aligned_bss ((FILE), (DECL), (NAME), (SIZE), (ALIGN))

#undef  ASM_OUTPUT_ALIGNED_BSS
#define ASM_OUTPUT_ALIGNED_BSS(FILE, DECL, NAME, SIZE, ALIGN) \
  c33_output_aligned_bss (FILE, DECL, NAME, SIZE, ALIGN)

/* This says how to output the assembler to define a global
   uninitialized, common symbol.  */
#undef  ASM_OUTPUT_ALIGNED_COMMON
#undef  ASM_OUTPUT_COMMON
#define ASM_OUTPUT_ALIGNED_DECL_COMMON(FILE, DECL, NAME, SIZE, ALIGN) \
     c33_output_common (FILE, DECL, NAME, SIZE, ALIGN)

/* This says how to output the assembler to define a local
   uninitialized symbol.  */
#undef  ASM_OUTPUT_ALIGNED_LOCAL
#undef  ASM_OUTPUT_LOCAL
#define ASM_OUTPUT_ALIGNED_DECL_LOCAL(FILE, DECL, NAME, SIZE, ALIGN) \
     c33_output_local (FILE, DECL, NAME, SIZE, ALIGN)

/* Globalizing directive for a label.  */
#define GLOBAL_ASM_OP "\t.global "

#define ASM_PN_FORMAT "%s___%lu"

/* This is how we tell the assembler that two symbols have the same value.  */

#define ASM_OUTPUT_DEF(FILE,NAME1,NAME2) \
  do { assemble_name(FILE, NAME1); 	 \
       fputs(" = ", FILE);		 \
       assemble_name(FILE, NAME2);	 \
       fputc('\n', FILE); } while (0)


/* How to refer to registers in assembler output.
   This sequence is indexed by compiler's hard-register-number (see above).  */

/* The assembler spells registers with a leading '%' (core manual 2.9), which
   c33_print_operand emits; these are the bare names.  */

/* The C33 assembler requires a '%' on every register name -- "add %r4,%r5",
   not "add r4,r5".  Carrying the prefix in REGISTER_NAMES rather than in each
   template is what the original toolchain's output looks like, and it means
   an operand printed with %0 comes out right without per-pattern help.

   REGISTER_PREFIX lets asm() register operands be written either way:
   strip_reg_name in varasm.cc drops the prefix before matching.  */

#define REGISTER_PREFIX "%"

#define REGISTER_NAMES						\
{ "%r0",  "%r1",  "%r2",  "%r3",  "%r4",  "%r5",  "%r6",  "%r7", \
  "%r8",  "%r9", "%r10", "%r11", "%r12", "%r13", "%r14", "%r15", \
  "%sp", "%alr", "%ahr",  "cc",  ".fp",  ".ap" }

/* Register numbers */

/* Aliases accepted in asm register names.  The C33 has no hardwired zero
   register and no link register, so the V850 names for those are gone; %dp
   is the conventional name for the default data area pointer.  */

#define ADDITIONAL_REGISTER_NAMES              \
{ { "dp",      15 } }

/* %r15 is the base register for the default data area (__dp).  See
   ABI.md; -medda32 turns data-area addressing off entirely.  */
#define C33_DP_REGNUM 15

/* The reach of a single add/sub %sp,imm10.  The immediate is scaled by 4 and
   the two forms cannot be ext-extended (core manual p64, both extension rows
   read "Unusable"), so 1023 * 4 bytes is the most one instruction can move
   the stack pointer.  Anything larger goes through add_sp_big.  */
#define C33_MAX_SP_ADJUST (1023 * 4)

/* The callee-saved range, %r0-%r3 (ABI.md).  */
#define C33_FIRST_SAVED_REG 0
#define C33_LAST_SAVED_REG  3

/* This is how to output an element of a case-vector that is absolute.  */

/* Jump tables hold absolute 4-byte addresses.

   Not 2-byte differences, which is what this was inherited doing: the EPSON
   assembler emits 0 for a .short whose value is the difference of two labels
   when the labels come *later* in the file -- which is always the case for a
   jump table, whose entries point forward at the case bodies.  The bug is in
   the original assembler too, so the 3.3.2 backend sidestepped it the same
   way, with CASE_VECTOR_MODE Pmode and .long entries.

   It is worth fixing in gas eventually; until then this is the shape that
   works, and it costs two bytes per case.  */

#define ASM_OUTPUT_ADDR_VEC_ELT(FILE, VALUE) \
  fprintf (FILE, "\t.long .L%d\n", VALUE)

/* This is how to output an element of a case-vector that is relative.  */

/* Disable the shift, which is for the currently disabled "switch"
   opcode.  Se casesi in c33.md.  */

/* No ASM_OUTPUT_ADDR_DIFF_ELT: CASE_VECTOR_PC_RELATIVE is not defined, so
   GCC never asks for a difference vector.  */

#define ASM_OUTPUT_ALIGN(FILE, LOG)	\
  if ((LOG) != 0)			\
    fprintf (FILE, "\t.align %d\n", (LOG))

/* Use dwarf2 debugging info by default.  */
#undef  PREFERRED_DEBUGGING_TYPE
#define PREFERRED_DEBUGGING_TYPE   DWARF2_DEBUG
#define DWARF2_DEBUGGING_INFO	   1

#define DWARF2_FRAME_INFO          1
#define DWARF2_UNWIND_INFO         0
/* On entry the return address is the word at the top of the stack, put
   there by call (core manual 2.4.4).  There is no register holding it, so
   the DWARF return column is a fake one past the real registers.  */

#define INCOMING_RETURN_ADDR_RTX   gen_rtx_MEM (Pmode, stack_pointer_rtx)
#define DWARF_FRAME_RETURN_COLUMN  FIRST_PSEUDO_REGISTER

#ifndef ASM_GENERATE_INTERNAL_LABEL
#define ASM_GENERATE_INTERNAL_LABEL(STRING, PREFIX, NUM)  \
  sprintf (STRING, "*.%s%u", PREFIX, (unsigned int)(NUM))
#endif

/* Specify the machine mode that this machine uses
   for the index in the tablejump instruction.  */
#define CASE_VECTOR_MODE Pmode

/* Table entries are absolute addresses, so CASE_VECTOR_PC_RELATIVE stays
   undefined -- see ASM_OUTPUT_ADDR_VEC_ELT above for why.  */

/* The switch instruction requires that the jump table immediately follow
   it.  */
#define JUMP_TABLES_IN_TEXT_SECTION (!TARGET_JUMP_TABLES_IN_DATA_SECTION)

#undef ASM_OUTPUT_BEFORE_CASE_LABEL
#define ASM_OUTPUT_BEFORE_CASE_LABEL(FILE,PREFIX,NUM,TABLE) \
  ASM_OUTPUT_ALIGN ((FILE), (TARGET_BIG_SWITCH ? 2 : 1))

#define WORD_REGISTER_OPERATIONS 1

/* Byte and short loads sign extend the value to a word.  */
#define LOAD_EXTEND_OP(MODE) SIGN_EXTEND

/* Max number of bytes we can move from memory to memory
   in one reasonably fast instruction.  */
#define MOVE_MAX	4

/* Define if shifts truncate the shift count
   which implies one can omit a sign-extension or zero-extension
   of a shift count.  */
#define SHIFT_COUNT_TRUNCATED 1

/* Specify the machine mode that pointers have.
   After generation of rtl, the compiler makes no further distinction
   between pointers and any other objects of this machine mode.  */
#define Pmode SImode

/* A function address in a call instruction
   is a byte address (for indexing purposes)
   so give the MEM rtx a byte's mode.  */
#define FUNCTION_MODE QImode

/* Tell compiler we want to support GHS pragmas */
#define REGISTER_TARGET_PRAGMAS() do {				\
  c_register_pragma ("ghs", "interrupt", ghs_pragma_interrupt);	\
  c_register_pragma ("ghs", "section",   ghs_pragma_section);	\
  c_register_pragma ("ghs", "starttda",  ghs_pragma_starttda);	\
  c_register_pragma ("ghs", "startsda",  ghs_pragma_startsda);	\
  c_register_pragma ("ghs", "startzda",  ghs_pragma_startzda);	\
  c_register_pragma ("ghs", "endtda",    ghs_pragma_endtda);	\
  c_register_pragma ("ghs", "endsda",    ghs_pragma_endsda);	\
  c_register_pragma ("ghs", "endzda",    ghs_pragma_endzda);	\
} while (0)

/* enum GHS_SECTION_KIND is an enumeration of the kinds of sections that
   can appear in the "ghs section" pragma.  These names are used to index
   into the GHS_default_section_names[] and GHS_current_section_names[]
   that are defined in c33.cc, and so the ordering of each must remain
   consistent.

   These arrays give the default and current names for each kind of
   section defined by the GHS pragmas.  The current names can be changed
   by the "ghs section" pragma.  If the current names are null, use
   the default names.  Note that the two arrays have different types.

   For the *normal* section kinds (like .data, .text, etc.) we do not
   want to explicitly force the name of these sections, but would rather
   let the linker (or at least the back end) choose the name of the
   section, UNLESS the user has forced a specific name for these section
   kinds.  To accomplish this set the name in ghs_default_section_names
   to null.  */

enum GHS_section_kind
{
  GHS_SECTION_KIND_DEFAULT,

  GHS_SECTION_KIND_TEXT,
  GHS_SECTION_KIND_DATA,
  GHS_SECTION_KIND_RODATA,
  GHS_SECTION_KIND_BSS,
  GHS_SECTION_KIND_SDATA,
  GHS_SECTION_KIND_ROSDATA,
  GHS_SECTION_KIND_TDATA,
  GHS_SECTION_KIND_ZDATA,
  GHS_SECTION_KIND_ROZDATA,

  COUNT_OF_GHS_SECTION_KINDS  /* must be last */
};

/* The following code is for handling pragmas supported by the
   c33 compiler produced by Green Hills Software.  This is at
   the specific request of a customer.  */

typedef struct data_area_stack_element
{
  struct data_area_stack_element * prev;
  c33_data_area                   data_area; /* Current default data area.  */
} data_area_stack_element;

/* Track the current data area set by the
   data area pragma (which can be nested).  */
extern data_area_stack_element * data_area_stack;

/* Names of the various data areas used on the c33.  */
extern const char * GHS_default_section_names [(int) COUNT_OF_GHS_SECTION_KINDS];
extern const char * GHS_current_section_names [(int) COUNT_OF_GHS_SECTION_KINDS];

/* The assembler op to start the file.  */

#define FILE_ASM_OP "\t.file\n"

/* Implement ZDA, TDA, and SDA */


#define SYMBOL_FLAG_ZDA		(SYMBOL_FLAG_MACH_DEP << 0)
#define SYMBOL_FLAG_TDA		(SYMBOL_FLAG_MACH_DEP << 1)
#define SYMBOL_FLAG_SDA		(SYMBOL_FLAG_MACH_DEP << 2)
#define SYMBOL_REF_ZDA_P(X)	((SYMBOL_REF_FLAGS (X) & SYMBOL_FLAG_ZDA) != 0)
#define SYMBOL_REF_TDA_P(X)	((SYMBOL_REF_FLAGS (X) & SYMBOL_FLAG_TDA) != 0)
#define SYMBOL_REF_SDA_P(X)	((SYMBOL_REF_FLAGS (X) & SYMBOL_FLAG_SDA) != 0)

#define TARGET_ASM_INIT_SECTIONS c33_asm_init_sections

#define ADJUST_INSN_LENGTH(INSN, LENGTH) \
  ((LENGTH) = c33_adjust_insn_length ((INSN), (LENGTH)))

#endif /* ! GCC_C33_H */
