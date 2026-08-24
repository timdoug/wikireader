# C33 GCC backend

Target: **GCC 16.2**. See [`ABI.md`](ABI.md) for the ABI and ISA specification
this is being written against.

## Status: builds; moves, calls and stack frames are real C33 code

`cc1` builds clean and the LRA blocker is gone. Everything in the test suite
below compiles, including calls, incoming stack arguments and frames too deep
for a single `sub %sp,imm10`:

```
callit:                 arg5:                   bigframe:
	xcall	g               xld.w	%r4,[%sp+4]         ld.w	%r14,%sp
	add 1,%r4               ret                     xsub	%r14,8000
	ret                                             ld.w	%sp,%r14
                                                        ...
```

The move, addressing and call machinery now emits genuine C33, and it
assembles and disassembles back to what we meant. Arithmetic, shifts,
comparisons and branches are still V850 -- `add 1,%r4` above should be
`add %r4,1` -- which is the rest of step 4.

| Test | |
|---|---|
| `int f(int a,int b,int c){return a+b+c;}` | OK |
| `int f(int a){return a*3;}` | OK |
| `int f(int *p){return *p;}` | OK |
| `int f(int *p,int i){return p[i];}` | OK |
| `int f(int a){int x[2]; x[0]=a; return x[0];}` | OK |
| `int f(int a){return a<0?-a:a;}` | OK |
| `int f(int a,int b,int c,int d,int e){return e;}` | OK |
| `extern int g(int); int f(int a){return g(a)+1;}` | OK |
| `int f(int a,...6 args...){return sum;}` | OK |
| `int f(int a){volatile char buf[8000]; ...}` | OK |

### The LRA bug, and what it actually was

The previous note here guessed that the arg pointer was being forced into a
pseudo during expand. It was not. The real cause was a missing register
class.

`%sp` is architecturally a *system* register on the C33, not one of `%r0`-`%r15`,
so this port had left it out of `GENERAL_REGS` -- and `BASE_REG_CLASS` was
`GENERAL_REGS`. But `[%sp+imm6]` is a perfectly good address, and LRA decides
whether an eliminable register may be a base by folding it to its elimination
target and asking for class membership: `in_class_p` calls
`lra_eliminate_reg_if_possible`, which substitutes `ep->to_rtx` and **drops the
offset**. So the question LRA asked about `[.ap + 4]` was "is `%sp` in the base
class?", the answer was no, and it reloaded the base into a pseudo. The reload
insn `r36 = .ap` then failed the same test for the same reason, and it recursed
until it hit the 90-reload limit.

The 3.3.2 backend had this right: a `SP_REGS` class holding just `%sp`, and
`BASE_REGS` as the union with `GENERAL_REGS`. That structure is now restored,
along with the `f` (`SP_REGS`) and `b` (`BASE_REGS`) constraint letters.

Moving `%sp` needs instructions too, and they exist: `ld.w %rd,%sp` and
`ld.w %sp,%rs` are the special-register forms (`RD,SS` and `SD,RS2`), two
bytes each. They are alternatives of `*movsi_internal` rather than separate
patterns -- as separate patterns with `match_operand` predicates they had the
same shape as any register move, won recog for every reg-to-reg copy, and then
failed constraint checking.

### Done since

* **Register classes**: `SP_REGS` and `BASE_REGS` added, `BASE_REG_CLASS` is
  now `BASE_REGS`, `REGNO_REG_CLASS` reports `SP_REGS` for `%sp`.
* **Register names carry the `%` prefix**, as the C33 assembler requires.
  `REGISTER_PREFIX` is defined so `asm()` operands may be written either way.
* **`output_move_single` rewritten** for the C33's single suffixed `ld`
  instruction: destination first, `ld.w %rd,%rs` for a copy, `[%rb]`,
  `[%rb]+` and `[%rb+disp]` for memory, `xld.w` where the operand may need
  `ext` prefixes. The V850's `mov`/`movea`/`movhi`/`st` are gone, as is the
  `%.` zero register, which this target does not have.
* **`c33_print_operand_address` rewritten**: `%sp+4`, not `4[sp]`. Brackets
  belong to the template, matching the 3.3.2 backend.
* **No more HIGH/LO_SUM splitting.** `xld.w %rd,imm32` takes the whole 32-bit
  range, so `movsi_source_operand` is just `general_operand` now.
* **Call patterns rewritten**: `scall`/`xcall` for a symbol, `call %rb`
  indirect, and no clobber -- the C33 pushes the return address on the stack,
  and V850's `(clobber (reg:SI 31))` named a *pseudo* here, which postreload
  rejects outright. `-mlong-calls` now selects the wider instruction instead
  of forcing the address into a register.
* **Frames deeper than 4092 bytes** go through `add_sp_big`, which is
  `ld.w %r14,%sp` / `xadd %r14,n` / `ld.w %sp,%r14`. The previous
  `add_sp_reg` emitted `add %rN,%sp`, which is not an instruction.
* **V850 interrupt machinery deleted** (~290 lines): `callt_save_interrupt`,
  `save_all_interrupt` and the rest were for its `ep`/`gp`/`callt` model and
  its 32 registers, named registers that do not exist here, and were
  unreachable -- nothing in `c33.cc` ever generated them. Replaced with a
  `reti` pattern, which the epilogue now uses for interrupt handlers.

### Still V850, and next

Arithmetic, logic, shifts, comparisons and branches. The operand order is the
thing to watch: C33 `add %rd,%rs` is `rd += rs`, the reverse of V850's
`add reg1,reg2`, and the immediate forms are `add %rd,imm` rather than
`add imm,%rd`. From `ABI.md`, extended register-to-register ops have
*different data flow* -- `ext imm13; add %rd,%rs` is `rd = rs + imm13`, not
`rd += rs` -- so they cannot be a length variant of the register form.

Also still V850: `negsi2` emits `subr %r0,%0`, which assumes a hardwired zero
register this target does not have.

### Done so far

* `c33-epson-elf` registered in `gcc/config.gcc`; `cc1` and `xgcc` built and
  ran end-to-end before the register conversion started, so the build loop is
  known good.
* **Register model converted** to the C33's, per core manual tables 2.9.1.1 and
  2.9.2.1: `FIRST_PSEUDO_REGISTER` 36 -> 22, holding `%r0`-`%r15`, `%sp`,
  `%alr`, `%ahr`, a `CC` register for `MODE_CC`, and the two virtual pointers.
  `FIXED_REGISTERS`, `CALL_USED_REGISTERS`, `REG_ALLOC_ORDER`,
  `REG_CLASS_CONTENTS`, `REGISTER_NAMES` and `REGNO_OK_FOR_BASE_P` all follow
  the ABI in `ABI.md`.
* Only `%r15` is reserved among the general registers, as the data area
  pointer. `%r10`-`%r14` are allocatable, where the 3.3.2 backend fixed them -
  they are call-clobbered in the original ABI and samo-lib's hand-written
  assembly only uses them as scratch inside a routine, so this changes no
  interface and takes us from 10 usable registers to 15.
* **FPU support removed.** The C33 has no FPU; floating point is entirely
  soft-float through libgcc (`__addsf3`, `__adddf3`, ...). Dropped the
  `CC_FPU_*` modes, ~740 lines of hardware-float patterns from `c33.md`, the
  float comparison predicates, and `c33_gen_float_compare`. `c33.md` is down
  from 3185 to 2429 lines.
* **Return-address macros corrected** for a stack-based return:
  `INCOMING_RETURN_ADDR_RTX` is now `gen_rtx_MEM (Pmode, stack_pointer_rtx)`,
  `EPILOGUE_USES` is 0, and `DWARF_FRAME_RETURN_COLUMN` is a fake column past
  the real registers.

* **Prologue/epilogue rewritten** for the C33's stack-based return.
  `compute_register_save_size`, `expand_prologue` and `expand_epilogue` were
  replaced rather than patched - the V850 originals were built around a link
  register, out-of-line `__save_xx`/`__restore_xx` helpers and PREPARE/DISPOSE,
  none of which the C33 has. The new ones save the callee-saved block with a
  single `pushn %rN` / `popn %rN` and adjust `%sp` with the dedicated
  `add/sub %sp,imm10` form, falling back to a scratch register and
  `ld.w %sp,%rs` past the 4092-byte reach.
* Removed V850's out-of-line prologue helpers (`construct_save_jarl`,
  `construct_restore_jr`) and PREPARE/DISPOSE (`construct_prepare_instruction`,
  `construct_dispose_instruction`), with their `.md` patterns.
* `return_internal`/`return_simple` emit `ret`; `USER_LABEL_PREFIX` is now
  empty, matching what samo-lib's assembly declares (`.global exit`, not
  `_exit`).
* `c33_return_addr` reads the stack slot instead of a link register.
* **Addressing model rewritten** for the C33's actual modes (core manual 5.5):
  `[%rb]`, `[%rb]+`, and base+displacement - the last covering `[%sp+imm6]`
  unextended and the 13/26-bit `ext` forms, since the assembler synthesises
  the prefixes. `REGNO_OK_FOR_BASE_P` now admits `%sp` and the two virtual
  pointers, which it must: frame slots are `%sp`-relative.
* `INITIAL_ELIMINATION_OFFSET` for the arg pointer now includes the
  return-address word that `call` pushes - the V850, which keeps the return
  address in `r31`, has no such word.
* `EVEN_REGS` is now identical to `GENERAL_REGS`. The C33 has no
  even-alignment requirement on 64-bit register pairs, and leaving it a strict
  subset makes LRA narrow reloads to a class the arg pointer cannot reach.
* `ep_memory_operand` is **disabled** (returns false). It is V850's
  `ep`-relative short addressing; the C33 analogue is `%r15`-relative
  default-data-area addressing, which belongs to step 5 and needs a proper
  address predicate rather than this one.

For reference, the output before conversion began - pure V850:

```
_add3:
	add r7,r6
	mov r6,r10
	add r8,r10
	jmp [r31]
```

### Two traps worth knowing about

Both cost real time and would bite anyone repeating this:

* **`%sp` is not a general register.** It is regno 16, outside `GENERAL_REGS`,
  so `gen_addsi3` on it matches no constraint. That is correct for the C33,
  which has dedicated `add/sub %sp,imm10`; the port now has `add_sp_imm` and
  `add_sp_reg` patterns for it.
* **The virtual frame and arg pointers must report `GENERAL_REGS`** from
  `REGNO_REG_CLASS`, and be members of it in `REG_CLASS_CONTENTS`. They appear
  in ordinary insns until reload eliminates them, so patterns have to accept
  them. Reporting `NO_REGS` - which looks right, since they are not real
  registers - produces `unrecognizable insn (set (reg) (reg .fp))` in reload.
  V850 gets this right by returning `GENERAL_REGS` for everything except the
  condition-code registers.

## Why V850 is the base

The 3.3.2 C33 backend is a V850 fork - the sources say so repeatedly ("Quoted
from v850", "According to V850"). V850 is still maintained upstream and still
carries the two things that are hardest to write from scratch for this target:
the small data area machinery and `-mlong-calls`. Its `v850-modes.def` also
defines exactly the `CCZ`/`CCNZ` pair that C33's PSR needs.

The fork is heavily diverged (only 7% of `c33.c` is verbatim V850), so this is
not a rebase - V850 supplies the *architecture of the solutions*, and the C33
specifics get written on top. See the fork analysis in `ABI.md`.

## Building

```sh
tools/gcc-glue.py <gcc-source-tree>     # registers c33 in config.gcc
# copy files/ over the tree, then:
../configure --target=c33-epson-elf --enable-languages=c \
             --without-headers --with-newlib --disable-libssp ...
make all-gcc
```

The C33 assembler must be on `PATH` - build binutils first
(`../binutils/build.sh`).

Two files GCC needs that are easy to forget, because they live outside
`gcc/config/`: `gcc/common/config/c33/c33-common.cc` and
`gcc/config/c33/c33.opt.urls`. Both are in `files/`.

## Next steps, in dependency order

1. ~~**Registers.**~~ Done - see above.
2. ~~**Return mechanism.**~~ Done - see above.
3. **`c33.opt`.** Swap in the option set drafted in `c33.opt.planned`, renaming
   the `TARGET_*` masks it removes throughout `c33.cc`/`c33.h`. Delete V850's
   `e1`/`e2`/`e3v5` core variants.
4. **`c33.md` - the bulk of the remaining work.** Moves, addressing, calls,
   the frame and `%`-prefixed register syntax are done. What is left is
   arithmetic, logic, shifts, comparisons and branches: C33 mnemonics and
   operand order (`add %rd,%rs` is `rd += rs`, the reverse of V850's
   `add reg1,reg2`; immediates are `add %rd,imm`, not `add imm,%rd`), and the
   `ext` prefix forms as separate patterns. Note from `ABI.md` that extended
   register-to-register ops have *different data flow* (`ext imm13;
   add %rd,%rs` is `rd = rs + imm13`, not `rd += rs`), so they cannot be a
   length variant of the register form. `negsi2` still emits `subr %r0,%0`,
   which assumes a hardwired zero register this target does not have.
5. **Data areas.** Retarget V850's `__gp`-relative addressing to C33's
   `%r15`-relative default data area, with `-medda32` selecting absolute
   addressing instead.
6. **Delay slots.** V850 has none; C33 has one non-annulling slot. Add
   `define_delay` - `or1k.md` has the identical shape.
7. **Assembler output.** No leading underscore on symbols; `;` comments; and
   emit `.size NAME,.-NAME` correctly (the 3.3.2 backend emitted
   `.size .NAME,.-.NAME`, which old gas silently mishandled - see the main
   README).

## Testing

The original compiler is available as an oracle at
`host-tools/toolchain-install/bin/c33-epson-elf-gcc`, and 143 files under
`samo-lib` and `wiki` compile with it.

Do not expect byte-identical output - 20+ years of optimiser changes make that
unrealistic. Use the oracle for **ABI conformance**: argument registers, frame
layout, struct passing, callee-saved sets. `ABI.md` lists the probe programs
that pin each of those down. For correctness, run the output under the
emulator in `emulator/`.
