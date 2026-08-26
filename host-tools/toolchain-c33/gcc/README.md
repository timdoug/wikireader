# C33 GCC backend

Target: **GCC 16.2**. See [`ABI.md`](ABI.md) for the ABI and ISA specification
this is being written against.

## Status: runs the WikiReader firmware; measured against gcc 3.3.2

The whole firmware -- kernel *and* `wiki.app` -- builds with gcc 16.2 and runs
in `emulator/`, booting from `flash.rom` off a FAT32 card.

**Rendering is bit-exact.** Search results, article view and scrolled article
are all pixel-identical to the gcc 3.3.2 build (0.00% residual once scroll
offset is accounted for).

### Size

| | gcc 3.3.2 | gcc 16.2 | |
|---|---:|---:|---|
| `wiki.app` stripped | 170,976 | 144,020 | -16% |
| `kernel.elf` stripped | 35,412 | 31,956 | -10% |
| 170 files at `-Os`, static | 113,465 insns | 105,779 | -6.8% |
| - memory operations | 38,102 | 35,193 | -7.6% |

### Speed

| workload | gcc 3.3.2 | gcc 16.2 | |
|---|---:|---:|---|
| boot + app load + first render | 146.1M cyc | 140.1M cyc | 4.1% faster |
| article load + render (wall) | 53.4M cyc | 59.6M cyc | 11.7% *longer* |

The article-load number is **not a code-generation regression**. Tracing
syscalls across the load shows every call that does real work is identical to
the call:

| syscall | gcc 3.3.2 | gcc 16.2 |
|---|---:|---:|
| `lcd_set_pixel` | 15,824 | 15,824 |
| `file_read` | 135 | 135 |
| `memory_allocate` | 29 | 29 |
| `lcd_framebuffer_get`/`set_byte` | 608 / 608 | 608 / 608 |
| `directory_exists` | 40 | 40 |
| **`timer_get`** | 282,557 | **612,827** |
| **`event_get`** | 96,422 | **207,850** |

The only difference is polling, and it is a clean 2.17x on both poll calls.
That is `Event_wait`'s loop -- `Event_get`, `Suspend`, repeat -- spun more
times *because the loop body is faster*, while the external event it waits on
(SD I/O, a timer tick) takes the same wall time either way.

The PC profile agrees: the idle path is ~29% of the window for this compiler
(`Suspend` 18.8%, timer 6.4%, `delay_us` 4.1%) against ~17% for gcc 3.3.2,
while *app* code is a smaller share (22.6% vs 28.1%). More time waiting, less
time working.

So the wall-clock figure is measuring the idle loop, not the compiler. A fair
comparison needs cycles spent outside `Suspend`/`Event_wait`, which the
current tooling does not separate cleanly.

**One thing worth chasing.** `memchr` (3.4%) and `memset` (2.9%) are in this
compiler's top twelve buckets and not in gcc 3.3.2's. That is 6.3% of the
window in two mini-libc routines, and it is the one place the profile
suggests a genuine codegen difference rather than an artefact.

### Optimisation levels

`-Os` wins outright and is what the firmware uses: smallest *and* fastest
to idle. `-O3` costs 41% more code for nothing measurable. **`-O1` is
broken** -- the kernel jumps to `pc=0x12` with a wild stack before printing
anything. That is a real backend bug, still un-diagnosed.

### A firmware quirk this surfaced

Scroll distance depends on CPU speed: the same scripted drag scrolls ~26%
further on the faster build, consistently across a 6x range of drag lengths.
The app appears to scroll a fixed increment per touch event processed rather
than tracking touch position, so a faster CPU changes the feel. Worth
knowing independently of this port.

### Three bugs that only running could find

Compiling, assembling and linking all succeeded while every one of these was
present.

* **`main` was not at the entry point.** gcc 4 and later split functions into
  `.text.startup`, `.text.unlikely` and friends; `grifo.lds` matched only
  `build/main.o(*.text)`, so `main` - which is what sets up `%sp` - went to
  `.text.startup` and some other function landed at `0x10000000`. The first
  `push` ran with `%sp` still zero. The script now takes
  `build/main.o(.text.startup .text.startup.*)` first.

* **Jump tables were emitted as zeroes.** This port inherited V850's
  2-byte PC-relative case vectors (`.short .Lx-.Ltab`). The EPSON assembler
  emits **0** for a `.short` whose value is a difference of labels that appear
  *later* in the file - which is always true of a jump table. Every `switch`
  therefore branched to the same place. The bug is in the original assembler
  too, which is why the 3.3.2 backend used `CASE_VECTOR_MODE Pmode` and
  absolute `.long` entries; this port now does the same. Worth fixing in gas
  eventually, but nothing has ever depended on the `.short` form working.

  This is what broke the touch input: the touch ISR's state machine ran
  `1,2,3,4,5,6` instead of `1,2,3,4,5,0`, never reaching the case that queues
  an event, so `Event_wait` blocked forever and the display never updated.

* Plus the `.text.startup` and linker-script issues above, neither of which
  any amount of static checking would have surfaced.

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

### Done in the instruction-set conversion

* **Arithmetic**: `add %rd,%rs` / `sub %rd,%rs`, two-operand with the source
  tied to the destination. Immediates go through `xadd`/`xsub`, which take a
  32-bit value; the immediate is *unsigned*, so a negative constant flips the
  mnemonic. `neg` is `not %rd,%rs` then `add %rd,1` -- there is no hardwired
  zero register to subtract from.
* **Logic**: `and`/`or`/`xor`/`not`, with `xand`/`xoor`/`xxor`/`xnot` for
  immediates. Note the spelling of `xoor`.
* **Shifts**: `sll`/`srl`/`sra` and their `x` forms.
* **Compare and branch**: `cmp %rd,%rs`, `xcmp` for an immediate, and
  `jr<cc>` / `sjr<cc>` / `xjr<cc>` selected by displacement range (2, 4 and 6
  bytes). Unconditional is `jp`/`sjp`/`xjp`. Unlike the V850 there is never a
  need to invert a condition and jump over an unconditional jump.
* **Extensions**: `ld` is a converting move -- the suffix gives the source
  width and signedness and the result fills the destination -- so
  `zero_extendqisi2` is one `ld.ub`, from a register or straight from memory.
  The V850 needed shift pairs and `zxb`/`sxh`; all of that is gone.
* **Multiply**: `mlt.w`/`mlt.h`/`mltu.h` into the `%ahr:%alr` pair, then
  `ld.w %rd,%alr` for the low half.
* **Divide**: no patterns at all, deliberately. The C33 divide is a
  multi-step `div0s`/`div1`/`div2s` sequence that does not fit one insn, and
  the original toolchain did not use it either -- patch 0003 in
  `host-tools/toolchain-patches` switches its libgcc to the C implementations.
  With no `divmodsi4`, GCC calls `__divsi3`.
* **Comments are `;`**, not `#`, including the `APP`/`NO_APP` markers around
  inline asm.
* **ALU immediates are `n`, not `i`.** With `i` a symbol could reach an
  immediate alternative and produce `xadd %r5,ButtonBuffer`, which is not an
  instruction.

Deleted rather than converted, because the C33 has no equivalent:

* `setf` and everything built on it -- `cstoresi4`, `*setcc_insn`, `*sasf`,
  and the whole `movsicc` family. GCC materialises these with a branch
  instead, which is what the 3.3.2 backend did.
* V850's `set1`/`clr1`/`not1`/`tst1` bit operations on memory. The C33 has
  `bset`/`bclr`/`bnot`/`btst`, but with a different operand shape (base
  register plus a 3-bit bit number), so they need writing rather than
  retemplating. Dropping them costs code size, not correctness -- GCC falls
  back to load/or/store. Worth revisiting.
* The `switch` instruction; `casesi` expands to a plain `tablejump`.
* ~290 lines of V850 interrupt machinery, and the `TARGET_C33E2_UP`
  three-operand shifts.

### Two ordering traps

Both cost real time, and both are consequences of `%sp` joining a register
class:

* **`register_operand` accepts `%sp` now**, so a generic `addsi3` will claim
  `(set (reg sp) (plus (reg sp) N))` and then fail constraint checking. The
  `add_sp_imm` patterns have to come *first* in `c33.md`. Generic code
  (argument pushing, alloca, stack probes) reaches the stack pointer through
  `gen_addsi3`, which after splitting is the same set wrapped in a parallel
  with a CC clobber, so that shape needs its own pattern too.
* **A pattern whose predicates match the same shape as a more general one
  wins recog if it comes first, then fails constraints.** Two `*movsi_from_sp`
  / `*movsi_to_sp` patterns written with `match_operand` and an `f` constraint
  looked specific but were not: their *predicates* were just
  `register_operand`, so they captured every register copy. Pin hard registers
  literally -- `(reg:SI SP_REGNUM)`.

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
3. ~~**`c33.opt`.**~~ Done. `-mc33`/`-mc33adv`/`-mc33pe` (aliases of
   `-mcore=`), `-medda32`, `-memcpy`, `-mlong-calls`. V850's `e1`/`e2`/`e3v5`
   core ladder is gone, along with `-mep`, `-mprolog-function`, `-mghs`,
   `-mgcc-abi` and the rest. The flags those masks fed are pinned to the
   value that is true for this target at the top of `c33.h`; simplifying the
   code that reads them is cleanup still owed.
4. ~~**`c33.md`.**~~ Done - see above. What is left of it is optimisation,
   not correctness: the `bset`/`bclr`/`btst` bit operations, and using the
   short unextended encodings where the operand provably fits (today we emit
   the `x` form and let the assembler narrow it, which is right but makes the
   `length` attribute pessimistic).
5. **Data areas.** Retarget V850's `__gp`-relative addressing to C33's
   `%r15`-relative default data area, with `-medda32` selecting absolute
   addressing instead.
6. **Delay slots.** V850 has none; C33 has one non-annulling slot. Add
   `define_delay` - `or1k.md` has the identical shape.
7. **Assembler output.** Symbols have no leading underscore and comments are
   `;` - both done. `.size NAME,.-NAME` comes out right, unlike the 3.3.2
   backend's `.size .NAME,.-.NAME` (see the main README).

8. **libgcc.** Done. Built as three multilibs, one per core, because the
   cores are not link-compatible -- the assembler stamps the variant into
   `e_flags` and the linker refuses to mix them. Integer division comes from
   GCC's own generic C implementations, which is what the original toolchain
   settled on too (patch 0003 in `host-tools/toolchain-patches`).

9. **Currently untested: does it run?** Everything so far is checked by
   compiling, assembling and linking. Nothing has been executed. The next
   milestone is running the output under `emulator/`.

## Testing

The original compiler is available as an oracle at
`host-tools/toolchain-install/bin/c33-epson-elf-gcc`, and 143 files under
`samo-lib` and `wiki` compile with it.

Do not expect byte-identical output - 20+ years of optimiser changes make that
unrealistic. Use the oracle for **ABI conformance**: argument registers, frame
layout, struct passing, callee-saved sets. `ABI.md` lists the probe programs
that pin each of those down. For correctness, run the output under the
emulator in `emulator/`.
