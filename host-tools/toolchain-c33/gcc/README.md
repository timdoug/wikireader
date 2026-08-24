# C33 GCC backend

Target: **GCC 16.2**. See [`ABI.md`](ABI.md) for the ABI and ISA specification
this is being written against.

## Status: builds; leaf functions correct, framed functions ICE

`cc1` builds clean, and **leaf functions now compile to correct C33 code**:

```
add3:                     mul3:
	add r7,r6         	mov r6,r4
	mov r6,r4         	shl 1,r4
	add r8,r4         	add r6,r4
	ret               	ret
```

Right ABI (`%r4` return, args in `%r6`+), right return (`ret`, no link
register), no leading underscore, correct `.size`. The mnemonics and register
syntax are still V850 - that is the `.md` rewrite, step 4.

Quite a lot compiles:

| Test | |
|---|---|
| `int f(int a,int b,int c){return a+b+c;}` | OK |
| `int f(int a){return a*3;}` | OK |
| `int f(int *p){return *p;}` | OK |
| `int f(int *p,int i){return p[i];}` | OK |
| `int f(int a){int x[2]; x[0]=a; return x[0];}` | OK |
| `int f(int a){return a<0?-a:a;}` | OK |
| `int f(int a,int b,int c,int d,int e){return e;}` | **ICE** |
| `extern int g(int); int f(int a){return g(a)+1;}` | **ICE** |

### The open bug

Both failures involve the *argument area* - reading incoming stack argument 5,
and making a call. Both die in LRA with

```
maximum number of generated reload insns per insn achieved (90)
```

The LRA dump shows why. The insn that matters is

```
(set (reg r4) (mem (plus (reg 36) (const_int 4))))     ; load arg 5
```

where pseudo 36 holds the **arg pointer**. LRA never eliminates `.ap`; instead
it repeatedly reloads it:

```
Choosing alt 0 in insn 89:  (0) =r  (1) Jr
Creating newreg=107 from oldreg=21, assigning class EVEN_REGS to r107
  89: r106:SI = r107:SI
  Inserting insn reload before:
  90: r107:SI = .ap:SI          <- needs its own reload, and so on
```

Ruled out so far, each tested and reverted or kept on its own merits:

* **not** `TARGET_CAN_ELIMINATE` - forcing it to return `true` unconditionally
  changes nothing.
* **not** the `Q`/`ep_memory_operand` constraint - disabling it entirely
  (see below) changes nothing.
* **not** the arg-pointer elimination offset, though that *was* genuinely
  wrong and is now fixed (it was missing the return-address word).
* **not** `EVEN_REGS` being a strict subset, though that too was a real bug
  and is fixed.

The remaining suspicion is that `.ap` is reaching constraint matching at all -
in a working port it should be eliminated to `%sp`/`%r3` plus a constant before
LRA starts picking alternatives. Worth checking `c33.md`'s `movsi_source_operand`
and the `addsi3` expander next: if the arg pointer gets forced into a pseudo
during expand (rather than left as `(plus (.ap) const)` for elimination to
fold), that would produce exactly this.

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
4. **`c33.md` - the current blocker, and the bulk of the remaining work.**
   C33 mnemonics and operand order (`add %rd,%rs` is `rd += rs`, the reverse
   of V850's `add reg1,reg2`), `%`-prefixed register syntax, and the `ext`
   prefix forms as separate patterns. This is also what has to happen before
   framed functions stop ICEing in reload. Note from `ABI.md` that extended register-to-register
   ops have *different data flow* (`ext imm13; add %rd,%rs` is
   `rd = rs + imm13`, not `rd += rs`), so they cannot be a length variant of
   the register form.
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
