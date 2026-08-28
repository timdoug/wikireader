# C33 ABI and code generation notes

Sources, in decreasing order of authority:

1. **The 3.3.2 backend's own source**, extracted in the tree at
   `host-tools/gcc-3.3.2/gcc/config/c33/`. `c33.c`'s `function_arg` and
   `c33.h`'s `FUNCTION_ARG_ADVANCE` *are* the calling convention, written out
   in about forty lines with EPSON's own numbered comments explaining each
   rule. Read them before probing anything.
2. **`s1c33.pdf`** - *S1C33 Family C33 PE Core Manual* (Epson, 182pp) in the
   repository root. The ISA reference: registers section 2, addressing modes section 5.5-5.6,
   branches section 5.14, per-instruction detail section 7. Section and page citations below
   refer to this. The manual documents the machine, not the ABI.
3. **Empirical probing** - compiling probe functions with the original
   `c33-epson-elf-gcc` 3.3.2 in `host-tools/toolchain-install/bin` and reading
   the generated assembly. Good for confirming a reading of (1); a poor way to
   *derive* the rules, because the interesting cases are the ones you do not
   think to write a probe for.

An earlier version of this file listed probing first and called the ABI
something that "must be derived by probing". That was wrong, and it cost
real time: the argument-passing rules were reconstructed one probe at a time,
generalised from single samples, and got two of them backwards before anyone
opened `c33.c`. Note that `grep` treats these files as binary - they contain
extended-ASCII - so `grep FUNCTION_ARG c33.h` silently finds nothing even
though the macro is right there. Use `python3` or `grep -a`.

`id001557.pdf` (*S1C33E07 Technical Manual*, 1015pp) covers the peripherals and
memory map - needed for the BSP and linker scripts, not for the backend.

Note the division of labour: the ABI (argument registers, struct passing,
callee-saved set) is a *software* convention that only the old compiler
defines, so it must be derived by probing. Everything about the *machine* -
flags, `ext` semantics, stack behaviour - comes from the manual and is stated
here with a citation.

The replacement backend must reproduce this ABI exactly: it has to interoperate
with `samo-lib`, `mini-libc`, grifo's hand-written assembly stubs, and the
prebuilt objects in `ROOT_IMAGE/`.

## Registers

16 general registers `%r0`-`%r15`, plus `%sp`, `%pc`, `%psr` and friends.
The 3.3.2 backend declares `FIRST_PSEUDO_REGISTER 19`: 0-15 are `%r0`-`%r15`,
16 is spare, 17 is the virtual arg pointer, 18 is the stack pointer.

| Registers | Role | Preserved across calls |
|---|---|---|
| `%r0`-`%r3` | callee-saved scratch | **yes** |
| `%r4`-`%r5` | return value (`%r4` low, `%r5` high) | no |
| `%r6`-`%r9` | first four arguments | no |
| `%r10`-`%r14` | fixed / reserved | - |
| `%r15` | **default data area pointer** (`__dp`) | - |
| `%sp` | stack pointer | - |

`%r9` doubles as the static chain register.

`%r15` being the data pointer is the single most important fact about this
target - see *Data areas* below.

## Calling convention

* Integer/pointer arguments 1-4 go in `%r6`-`%r9`; the rest go on the stack.
* `call` pushes the return address, so on entry incoming stack arguments start
  at `[%sp+4]`, not `[%sp]`:

  ```
  f_args(int a,int b,int c,int d,int e,int f)
      a..d -> %r6..%r9
      e    -> [%sp+4]
      f    -> [%sp+8]
  ```

* 64-bit scalars (`long long`, `double`) occupy register **pairs**, low word
  first. Returned in `%r4`/`%r5`.
* **There is no even-pair alignment.** A 64-bit argument starts at the next
  free word, not the next free *even* word:

  ```
  f(u32, unsigned long long, u32)   ->  %r6, %r7:%r8, %r9
  ```

  `function_arg` does contain an alignment step -- `nbytes` is rounded up to
  `TYPE_ALIGN (type)` -- but `BIGGEST_ALIGNMENT` is 32, so nothing on this
  target can ever have an alignment larger than a word and **the rounding is
  a no-op for every type**. Deriving the alignment from the argument's
  *size* instead, as V850 does, puts the pair in `%r8:%r9` and pushes the
  third argument onto the stack. That is invisible within one compilation --
  both halves agree with each other -- and only shows on a call to a
  3.3.2-built object.
* **Nothing is ever split between registers and the stack.** The 3.3.2
  backend does not define `FUNCTION_ARG_PARTIAL_NREGS` at all: an argument
  is wholly in registers or wholly in memory, and `function_arg` alone
  decides which. Any `TARGET_ARG_PARTIAL_BYTES` in the new backend must
  therefore return 0 unconditionally; a hook that computes its own answer
  will eventually contradict `function_arg`.
* **An argument starting in the last slot runs past `%r9`.** Since nothing
  is split, `function_arg` hands back a register of the argument's *full*
  mode, and a multi-word mode based at `%r9` simply continues into `%r10`
  and beyond. A `long long` takes `%r9:%r10`; a 16-byte `_Complex long
  double` based at `%r9` reaches `%r12`. A following *scalar* argument still
  goes on the stack, so these are not general argument registers -- but
  `FUNCTION_ARG_REGNO_P` still has to cover `%r6`-`%r12`, or the later
  passes do not know an incoming argument lives there. See the
  `-frename-registers` note below.
* **`double` is the exception, and only `double`.** A `DFmode` argument that
  would start in the last slot goes *wholly on the stack* and consumes no
  register at all, so the next argument still gets `%r9`. A `long long` in
  exactly the same position takes `%r9:%r10`. Same size, same slot, opposite
  rules -- `function_arg` special-cases `DFmode` and `FUNCTION_ARG_ADVANCE`
  matches it:

  ```
  f(int, int, int, long long d, int e)   ->  d in %r9:%r10,  e on the stack
  f(int, int, int, double    d, int e)   ->  d on the stack, e in %r9
  ```
* Floating point is **soft-float only**. `double a+b` compiles to a call to
  `__adddf3`, `float` to `__addsf3`, with the operands already in the integer
  argument registers.
* `char`/`short` arguments are promoted to `int`. Narrow return values are
  sign-extended by the callee (`ld.b %r4,%r4`).

### Structures

The register/stack boundary for an aggregate is **not a size limit**. It is
`function_arg`'s first test: `if (mode == BLKmode) return 0`. So the question
is only whether GCC gave the record a scalar mode, which `compute_record_mode`
does when the size matches a machine mode exactly. A 3-byte struct is BLKmode
and goes on the stack; a 4-byte struct is `SImode` and goes in a register.

| type | mode | where |
|---|---|---|
| `struct { char a,b,c; }` | BLK | stack |
| `struct { int a; }` | SI | one register |
| `struct { char a[5]; }` | BLK | stack |
| `struct { int a,b; }` | DI | register pair |
| `struct { int a; char b; }` | DI | register pair |
| `struct { int a,b,c; }` | BLK | stack |

* **A record with a scalar mode**: passed and returned in registers.
  `struct { int a, b; }` arrives in `%r6`/`%r7` and returns in `%r4`/`%r5`.
* **> 8 bytes, as a return value**: the caller supplies a hidden pointer to
  the return slot as the *first* argument in `%r6`, and the callee returns
  that same pointer in `%r4`. The 16-byte case observably uses `memcpy` to
  populate the slot.
* **> 8 bytes, as an argument**: copied onto the stack **by value** -- not by
  hidden pointer -- and it **consumes no argument register**. Scalars and
  large aggregates are two independent streams:

  ```
  h1(u32 a, struct S12 s, u32 b)  ->  a in %r6, s at [%sp+4], b in %r7
  h2(struct S16 s, u32 b)         ->  s at [%sp+4], b in %r6
  ```

  Note `b` in `%r7` in the first case: the struct did not take a register
  slot. This was originally documented as a hidden pointer, conflating it
  with the return convention above; the two are different.

### Status: implemented and verified

`tests/abi/run-abi.sh` cross-links the two compilers in all four
combinations and **all of them agree**, on 36 emitted values, at `-O0`,
`-O1`, `-O2`, `-Os` and `-O3 -funroll-loops`. The full `gcc.c-torture`
execute and compile suites pass at all seven option sets with zero failures,
and `wiki.app` built with the fixed compiler renders a byte-identical screen.

The whole convention is four hooks in `c33.cc`, and each one mirrors 3.3.2:

| hook | rule |
|---|---|
| `c33_pass_by_reference` | always false - 3.3.2 has no such hook |
| `c33_arg_partial_bytes` | always 0 - 3.3.2 has no `FUNCTION_ARG_PARTIAL_NREGS` |
| `c33_function_arg` | BLKmode -> stack; align by `TYPE_ALIGN` (a no-op); `DFmode` past slot 3 -> stack; else `%r6 + nbytes/4` |
| `c33_function_arg_advance` | BLKmode adds 0; `DFmode` past slot 3 adds 0; else the rounded mode size |

Three things made this look much harder than it was, and are worth
remembering:

* **The "spurious 8-byte alignment" was a misreading.** The alignment step
  is 3.3.2's own and is correct; what was wrong was our extra condition
  `size <= UNITS_PER_WORD && arg.type`, which sent every 8-byte typed scalar
  down the `align = size` path. The fix is to delete the condition, not the
  alignment.
* **The apparent self-contradiction between `function_arg` and
  `arg_partial_bytes` was not a design problem to solve.** 3.3.2 has no
  partial-bytes hook at all, so the answer is simply 0. An earlier attempt
  tried to make the two hooks agree by teaching `arg_partial_bytes` about
  straddling, which is a rule the ABI does not have.
* **The three "divergences" were symptoms of one cause.** They were derived
  separately by probing and looked like three unrelated special cases
  needing three separate fixes. In the source they are three lines of one
  function.

### `_Complex double` is no longer a divergence - and it retired the ICE

An earlier version of this file recorded `_Complex double` as a deliberate
divergence: 3.3.2 passes it by value in `%r6`-`%r9`, the new backend passed
it by reference, and that was called acceptable because nothing on the
device uses `_Complex`. Removing `c33_pass_by_reference` fixed it along with
everything else, and the four-combination cross-link confirms it.

That also silenced the only ICE in the compile suite. `pr110266` ICEd in
`expand_builtin_cexpi` at `expr.cc:9343`, and `tests/FAILURES.md` argued at
some length that it was upstream's bug, on the grounds that the 16-byte
`_Complex double` was passed *in memory*, so expand had to take the address
of a `COMPLEX_EXPR` rvalue and `get_inner_reference` cannot. The mechanism
was right; the conclusion was not. Passing it in memory was **our** bug, and
with the ABI fixed the value goes in registers and expand never needs its
address. `pr110266` now passes at all seven option sets.

The lesson is narrow and specific: an ICE reached through a target-dependent
path is not upstream's until the target's own behaviour on that path has
been checked against the oracle. "The ABI is right (>8 bytes to memory,
inherited from gcc 3.3.2)" was asserted without checking, and it was the one
load-bearing claim in the argument.

### Why `FUNCTION_ARG_REGNO_P` has to cover `%r6`-`%r12`

Getting the argument rules right made an old latent bug reachable, and it is
the kind that only appears under one optimisation flag.

`df` marks every register satisfying `FUNCTION_ARG_REGNO_P` as defined on
entry to the function; that is what tells the later passes an incoming
argument lives there. The macro said `6..9`, the documented set. But because
nothing is split, an argument based at `%r9` continues into `%r10` and past
it - so in `check_float (int, _Complex float a1, ..., a5)` the argument `a2`
arrives in `%r9:%r10`, and `-frename-registers` cheerfully took `%r10` as a
scratch:

```
	xld.w	%r10,[%sp+60]      ; regrename thinks %r10 is dead
	...
	ld.w	%r1,%r10           ; reads a2's high word -- now garbage
```

`complex-7` catches it. It aborts at `-O3 -funroll-loops` - which implies
`-frename-registers` - and passes at plain `-O3`, at `-O3 -fweb`, and at
`-O3 -funroll-loops -fno-rename-registers`. It is not about unrolling, and
`complex-7` contains no loops.

## Stack frame

*Authoritative source: core manual section 2.4, pp. 7-9, and section 7 p. 64.*

* `%sp`'s two low bits are **fixed at 0 and read-only** - the stack is always
  word-aligned by hardware.
* `push` is `SP -= 4` then store; `pop` is load then `SP += 4`.
* `pushn %rN` pushes `%r0`...`%rN` as a block (so `pushn %r2` does `SP -= 12`);
  `popn %rN` is the inverse. Saving all four callee-saved registers is
  `pushn %r3` / `popn %r3`.
* `call` does `SP -= 4; PC -> [SP]`, and `ret` does `[SP] -> PC; SP += 4`. The
  return address lives **on the stack**, not in a link register - which is why
  incoming stack arguments start at `[%sp+4]`.
* **`add %sp,imm10` and `sub %sp,imm10` scale the immediate by 4**:
  `sp <- sp + imm10 * 4`, with `imm10` zero-extended. The original compiler
  emits the byte count as a comment, which is the giveaway:

  ```
  sub %sp,4    ;16
  sub %sp,8    ;32
  ```

  An easy source of 4x frame-size bugs.
* These two forms are **not `ext`-extensible** (the manual lists both extension
  rows as "Unusable"), so a single instruction can only move `%sp` by at most
  1023 * 4 = **4092 bytes**. Larger frames need a scratch register and
  `ld.w %sp,%rs`.
* `add %sp,imm10` affects **no flags**, so it can be scheduled freely around a
  comparison.
* Interrupts push both PC and PSR (`SP -= 8`); `reti` pops both.
* Leaf functions that need no frame emit no prologue at all.

## Condition codes

*Authoritative source: core manual section 2.3, pp. 5-6.*

PSR carries a conventional NZCV set in its low bits, plus interrupt state:

| Bit | Flag | Set by |
|---|---|---|
| 0 | `N` | bit 31 of the result of a logical, arithmetic or shift op |
| 1 | `Z` | result is zero |
| 2 | `V` | signed overflow/underflow; **reset to 0 by logical ops** |
| 3 | `C` | unsigned carry/borrow |
| 4 | `IE` | interrupt enable |
| 11:8 | `IL` | interrupt level |

This maps cleanly onto GCC's `MODE_CC` scheme, which is what replaces the
`cc0` the 3.3.2 backend used:

* a full `CCmode` for `cmp`, giving signed and unsigned conditions
  (`jrgt`/`jrge`/`jrlt`/`jrle` and `jrugt`/`jruge`/`jrult`/`jrule`);
* a `CC_NZmode` for logical and shift results, where only `N` and `Z` are
  meaningful - `V` is forced to 0 and `C` is untouched, so unsigned
  comparisons must not be allowed to use it.

Getting that second mode right is what stops the compiler folding a `cmp`
into a preceding `and`/`or` and then branching on `C`.

## Data areas - the distinctive part

The C33 has several "data areas" addressed relative to a base register, so that
globals can be reached without materialising a full 32-bit address. GCC's
default mode uses the *default data area* based on `%r15`:

```c
int g; int rd(void) { return g; }
```
```
	ext doff_hi(g)
	ext doff_lo(g)
	ld.w	%r4,[%r15]
```

The two `ext` prefixes supply the offset from `%r15`; these become the
`R_C33_DH` / `R_C33_DL` relocation pair. Under `-medda32` ("no default data
area") the compiler instead materialises the absolute address:

```
	xld.w	%r4,g          ; R_C33_H / R_C33_M / R_C33_L triple
	ld.w	%r4,[%r4]
```

`samo-lib` builds C with the **default** (`%r15`-relative) mode -
`samo-lib/Mk/rules.mk` passes `-mc33pe -mlong-calls` and *not* `-medda32`. Both
modes must work.

Analogous `g`/`s`/`t`/`z` areas exist (`R_C33_GL`, `R_C33_SH/SL`,
`R_C33_TH/TL`, `R_C33_ZH/ZL`) selected by the `-mezda`/`-metda`/`-mesda`
switches and by section attributes. `samo-lib` does not appear to use them, so
they are lower priority than the default area.

## The `ext` prefix mechanism

*Authoritative source: core manual section 5.6, pp. 25-30.*

C33 instructions are 16 bits. Larger immediates come from up to **two**
preceding `ext imm13` instructions, each carrying 13 bits. Three or more `ext`
in a row raises an undefined instruction exception. An `ext` before an
instruction that does not accept extension executes as a `nop`.

```
xld.w %r4, 0x12345678
    ext 0x246      ; bits 31:19
    ext 0x1159     ; bits 18:6
    ld.w %r4,0x38  ; bits  5:0
```

The extension rules differ per operand class, and the differences matter:

| Operand | +1 `ext` | +2 `ext` |
|---|---|---|
| `imm6` | 19-bit, **zero**-extended | 32-bit: `imm13(1)`:`imm13(2)`:`imm6` |
| `sign6` | 19-bit, **sign**-extended from `imm13` MSB | 32-bit, signed |
| `[%rb]` | + 13-bit zero-extended displacement | + **26-bit** zero-extended displacement |
| `[%sp+imm6]` | 19-bit | 32-bit - low bits forced to 0 per transfer size |
| `sign8` (PC-relative) | 22-bit signed | 32-bit signed |

Three traps for the backend:

1. **`sign6` changes meaning when extended.** Unextended, the top bit of
   `sign6` is the sign. Extended, that bit is *data* and the sign comes from
   the `ext`. Same for `sign8` in branches.

2. **Extended register-to-register ops are not accumulate.** Plain
   `add %rd,%rs` is `rd += rs`. But `ext imm13; add %rd,%rs` is
   `rd = rs + imm13` - the manual is explicit that "the content of the `rd`
   register does not affect the arithmetic operation performed". The extended
   form is a *two-operand* instruction with a different data flow, so it needs
   its own `define_insn`, not a variant length on the register form.

3. **`[%sp+imm6]` is scaled by transfer size.** Unextended, `ld.h %rd,[%sp+imm6]`
   addresses `sp + imm6*2`, and `ld.w` addresses `sp + imm6*4`. When extended,
   the low bits are forced to zero instead. gas accepts byte offsets in the
   source and does the scaling, but frame-offset arithmetic in the backend must
   respect the resulting alignment restrictions.

PC-relative branch displacements are `sign8 x 2` (bit 0 always 0), extending to
22 or 32 bits.

Consequences:

* An `x`-prefixed mnemonic (`xld.w`, `xcall`, `xadd`, `xcmp`, ...) is an
  assembler *pseudo-instruction* that expands to `ext` + base insn. The backend
  emits the `x` form and lets gas encode it - it does **not** emit `ext`.
* Relaxation therefore happens in the assembler, not the compiler. gas's
  `ext_remove.c` is a second pass that strips redundant prefixes.
* Instruction lengths are variable, which `define_insn` `length` attributes
  must account for when GCC computes branch ranges.

## Calls

| Form | Emitted for | Relocations |
|---|---|---|
| `scall label` | default (short call) | `R_C33_S_RH/S_RM/S_RL` |
| `xcall label` | `-mlong-calls` | `R_C33_RH/RM/RL` |
| `call %rN` | indirect through a register | - |

`samo-lib` builds with `-mlong-calls`, so `xcall` is the common case.

## Delay slots

Branches and calls have **one non-annulling delay slot**, selected by the `.d`
mnemonic suffix:

```
	scall.d	g
	ld.w	%r1,%r6     <-- executes as part of the call
```

The 3.3.2 machine description models this minimally:

```
(define_delay (eq_attr "needs_delay_slot" "yes")
  [(eq_attr "in_delay_slot" "yes") (nil) (nil)])
```

One slot, no annul-true, no annul-false. The replacement backend can keep the
same shape; `define_delay` still exists in modern GCC.

## Assembler output requirements

* Emit `.size NAME,.-NAME`. The 3.3.2 backend emits `.size .NAME,.-.NAME` with
  a stray leading dot - binutils 2.10.1 accepted it and silently created a
  bogus *undefined* symbol carrying the size, leaving the real symbol at size
  zero. Modern gas rejects it. Do not reproduce this bug.
* Comments start with `;`.
* `-mc33pe` selects the PE core; `-mc33adv` the ADV core; plain `-mc33` the
  standard core.

## The C33 backend is a V850 fork - and V850 is still maintained

This is the single most useful structural fact about the port. EPSON derived
the C33 backend from NEC's V850 backend, and says so throughout:

```
/* C33: Quoted from v850.  */
/* C33: According to V850 for now.  */
        in gcc/config.gcc. ( reference to v850 )
```

The binutils side has the same ancestry - `include/opcode/c33.h` still opens
with `#ifndef V850_H` and carries commented-out `PROCESSOR_V850E` constants.

How close is the fork? Comparing the C33 files against the V850 files from the
*same* gcc 3.3.2 release, after normalising `v850`->`c33` in identifiers:

| File | V850 3.3.2 | C33 3.3.2 | Lines still verbatim |
|---|---|---|---|
| `.c` | 3443 | 4087 | 304 (7%) |
| `.h` | 1538 | 2348 | 832 (35%) |
| `.md` | 1961 | 1242 | 465 (37%) |

So it is a *heavily* modified fork, not a light rename - line-level rebasing
onto modern V850 is not viable. What carries over is the **architecture of the
solutions**, and modern V850 still has all of it:

* `v850-modes.def` defines `CCZ` and `CCNZ` - precisely the two-mode `MODE_CC`
  scheme C33's PSR needs (see *Condition codes*).
* The small data area machinery (`v850_data_area`, `DATA_AREA_SDA/TDA/ZDA`,
  the section-kind handling and `__gp`/`__ep` relative addressing) is intact.
  C33's "default data area" via `%r15` is the same idea with another base
  register.
* `-mlong-calls` already exists and means the same thing.
* Section attributes and the `ghs` pragma scaffolding are still there.

**Therefore**: base the new backend on modern `gcc/config/v850`, not on a
generic minimal target like `moxie` or `or1k`. V850 already solves the two
hardest C33-specific problems - data areas and long calls - and its delay-slot
`define_delay` has the same one-slot non-annulling shape C33 needs.

The C33-specific work on top of that base is: the 16-register file and its
ABI (above), the `ext` prefix mechanism, PSR flag semantics, the `%r15`
default data area, and deleting V850's `e1`/`e2`/`e3v5` core variants in favour
of `c33`/`c33adv`/`c33pe`.

## Things that no longer exist in modern GCC

Carried over from the earlier survey of the 3.3.2 backend, these are the
constructs that force a rewrite rather than a port:

| 3.3.2 construct | Modern replacement |
|---|---|
| `cc0`, `NOTICE_UPDATE_CC` | `MODE_CC` with an explicit CC register |
| reload | LRA (reload was removed in GCC 16) |
| `TARGET_SWITCHES` | `c33.opt` |
| `REG_CLASS_FROM_LETTER`, `CONST_OK_FOR_LETTER_P`, `EXTRA_CONSTRAINT` | `constraints.md` |
| `PREDICATE_CODES` | `predicates.md` |
| `GO_IF_LEGITIMATE_ADDRESS` | `TARGET_LEGITIMATE_ADDRESS_P` |
| `define_function_unit` | DFA pipeline description, or drop it |
| text prologues via `TARGET_ASM_FUNCTION_PROLOGUE` | RTL prologue/epilogue |
| STABS debug output | DWARF |

## Verifying the new backend

The original compiler is a byte-exact oracle. The 143 files under `samo-lib`
and `wiki` that gcc 3.3.2 can compile give a ready-made corpus:

```sh
OLD=host-tools/toolchain-install/bin/c33-epson-elf-gcc
$OLD -S -Os -mc33pe -mlong-calls -fno-builtin -o old.s file.c
# ...compile the same file with the new backend, assemble both, compare .text
```

Expect *some* divergence - 20+ years of optimiser changes mean instruction
selection and scheduling will differ. The oracle is most useful for checking
**ABI conformance** (argument registers, frame layout, struct passing,
callee-saved sets) rather than literal instruction equality. Link-and-run
against the emulator in `emulator/` is the stronger functional check.
