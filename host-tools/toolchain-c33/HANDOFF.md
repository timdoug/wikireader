# C33 toolchain modernisation - where things stand

Start here. `README.md` covers binutils in detail, `gcc/README.md` the
compiler, `gcc/ABI.md` the target specification.

## The goal

Replace the EPSON C33 toolchain (binutils 2.10.1 / gcc 3.3.2, from 2000-2002)
with current upstream releases, so the WikiReader can be built with a
toolchain that runs natively on a 64-bit host, emits DWARF instead of STABS,
and gives the `emulator/` work modern `objdump`/`readelf`.

## Where we are

| Component | State |
|---|---|
| binutils 2.47 - bfd, opcodes, gas, ld | **done and validated byte-for-byte** |
| GCC 16.2 backend | in progress; builds, moves/calls/frames emit real C33, arithmetic still V850 |

### binutils - finished

A complete `c33-epson-elf-*` toolchain builds and installs. It is validated
against the original as a byte-exact oracle:

* 86 hand-written `samo-lib/**/*.s`: `.text`, `.data` and relocations all
  byte-identical.
* 143 files compiled to `.s` by the original gcc 3.3.2: `.text` all
  byte-identical.
* 20 of those differ in relocation *representation* only (modern gas reduces
  local symbol references to `.text+offset`); linking both ways gives
  identical `.text`.

Reproduce with `tools/compare-with-oracle.sh`.

Three real bugs were fixed getting there, all of which produced *silently
wrong output* rather than errors - see "Fixed during the port" in `README.md`.
The worst was that all 32 relocation `HOWTO` entries still used the historical
log2 size encoding, so every relocation misreported its width and was quietly
discarded.

### GCC - in progress

Builds against GCC 16.2. The LRA blocker is fixed and everything we throw at
it now compiles: calls, incoming stack arguments, frames deeper than a single
`sub %sp,imm10`, local arrays, conditionals.

Moves, addressing, calls, the prologue/epilogue and register syntax emit
genuine C33, verified by assembling the output and disassembling it back.
Arithmetic, logic, shifts, comparisons and branches are still V850 - that is
the remainder of step 4.

`gcc/README.md` has the detail, including what the LRA bug actually was (a
missing `SP_REGS`/`BASE_REGS` class pair, not the arg-pointer hypothesis
recorded here previously).

## Getting a working tree back

Everything here is source and scripts; the build trees are disposable.

```sh
# the oracle (original toolchain) -- from the repo root
make toolchain                      # installs to host-tools/toolchain-install

# the new binutils
host-tools/toolchain-c33/binutils/build.sh

# check it against the oracle
host-tools/toolchain-c33/tools/compare-with-oracle.sh

# the new gcc (needs the new binutils on PATH; build.sh handles that)
host-tools/toolchain-c33/gcc/build.sh
```

The original toolchain builds on modern macOS thanks to the Darwin/ARM64
patches in `host-tools/toolchain-patches/` (`0004-binutils-*`, `0006-gcc-*`,
`0007-gcc-*`).

## Layout

```
binutils/
  build.sh              pristine tarball -> patched, configured, built
  files/                the C33-specific sources; shipped whole, not as patches
gcc/
  build.sh              same, for GCC 16.2
  ABI.md                the target spec: ABI, ISA, ext mechanism, PSR, frame
  README.md             backend status, what is done, what is next
  c33.opt.planned       drafted C33 option set, not yet swapped in
  files/                the backend, mid-conversion from V850
  probes/               the C programs used to derive the ABI from the oracle
tools/
  glue.py               registers c33 across binutils' shared files
  gcc-glue.py           same for GCC's config.gcc
  modernize.py          pre-ANSI C converter (PARAMS, K&R, boolean->bool)
  compare-with-oracle.sh  the byte-for-byte validation harness
tests/
  ext-encoding.s        ext prefix encoding check
  relocs.s, relocs-lib.s  cross-file relocation resolution
```

## Where we want to go

In dependency order. Steps 1-2 are done; see `gcc/README.md` for detail.

3. **`c33.opt`** - swap in `gcc/c33.opt.planned`, renaming the `TARGET_*`
   masks it drops throughout `c33.cc`/`c33.h`, and delete V850's `e1`/`e2`/
   `e3v5` core variants.
4. **`c33.md`** - the bulk of the work. Moves, addressing, calls, the frame
   and `%`-prefixed register syntax are done; arithmetic, logic, shifts,
   comparisons and branches are not. Watch the reversed operand order
   (`add %rd,%rs` is `rd += rs`, the opposite of V850's `add reg1,reg2`), and
   keep the `ext` forms as separate patterns because their data flow differs
   (`ext imm13; add %rd,%rs` is `rd = rs + imm13`, *not* `rd += rs`).
5. **Data areas** - retarget V850's `__gp`-relative addressing to C33's
   `%r15`-relative default data area, with `-medda32` selecting absolute
   addressing. `ep_memory_operand` is currently stubbed out and belongs here.
6. **Delay slots** - one non-annulling slot; `or1k.md` has the same shape.
7. **libgcc and a full build** - then build `samo-lib` end to end.

## How to test the compiler

The original compiler at `host-tools/toolchain-install/bin/c33-epson-elf-gcc`
is an oracle, but a weaker one than for the assembler: 20+ years of optimiser
changes mean instruction selection and scheduling will legitimately differ.

Use it for **ABI conformance** - argument registers, frame layout, struct
passing, callee-saved sets. `gcc/probes/` holds the programs that pin each of
those down, and `gcc/ABI.md` records what the original compiler does with
them. For correctness, run output under the emulator in `emulator/`.

## Things that bit us, so they do not bite again

* Comparing whole `.o` files against the oracle is meaningless - ELF headers,
  section order and symtab layout differ legitimately. Compare `.text`.
* The 3.3.2 compiler emits `.size .foo,.-.foo` with a stray leading dot.
  binutils 2.10.1 accepted it and invented a bogus undefined symbol carrying
  the size; modern gas rejects it. Every function in the shipped binaries has
  a wrong size entry as a result.
* `%sp` is not a general register on the C33 (regno 16 here), so generic
  `addsi3` on it matches no constraint. It has dedicated `add/sub %sp,imm10`.
* ...but `%sp` **must** still be in `BASE_REG_CLASS`, because `[%sp+imm6]` is a
  real address. LRA decides whether an eliminable register may be a base by
  folding it to its elimination target and testing class membership -
  `lra_eliminate_reg_if_possible` substitutes `ep->to_rtx` and drops the
  offset. Leave `%sp` out and `[.ap + N]` is judged invalid, LRA reloads the
  base, the reload fails the same test, and it recurses to the reload limit.
  The symptom looks nothing like the cause.
* A `define_insn` whose predicates match the same shape as a more general one
  wins recog if it comes first in the file, and then fails constraint
  checking. Pin hard registers literally - `(reg:SI SP_REGNUM)` - rather than
  via a `match_operand` with a narrow constraint.
* V850 patterns name hard registers up to 31. With `FIRST_PSEUDO_REGISTER` at
  22 those are *pseudo* numbers, and postreload asserts on a CLOBBER of a
  pseudo. Grep for out-of-range register numbers when porting a pattern.
* GCC's virtual frame and arg pointers must report `GENERAL_REGS`, even though
  they are not real registers, because they appear in insns until elimination.
* Several C33 source files contain non-ASCII bytes, so `grep` treats them as
  binary and silently reports nothing. Use `grep -a`. This cost real time
  twice: it hid `#include "ext_remove.h"` and two live functions, and led to
  removing working code on the assumption it was dead.
