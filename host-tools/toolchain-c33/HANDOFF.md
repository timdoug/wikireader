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
| GCC 16.2 backend | **runs the whole firmware**, output byte-identical to gcc 3.3.2; app -15%, 8.7% fewer instructions to boot, 86% fewer soft-float calls |

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

### GCC - runs the whole firmware, and is faster than 3.3.2

Kernel, `init.app` *and* `wiki.app` build with gcc 16.2 and run in
`emulator/`. After typing `LOVE` and tapping a result the framebuffer is
**byte-identical** to the gcc 3.3.2 build; the scrolled screen matches at
0.00% once its 31-pixel difference in scroll position is accounted for.

| | gcc 3.3.2 | gcc 16.2 | |
|---|---:|---:|---|
| `wiki.app` stripped | 170,352 | 143,992 | -15% |
| kernel stripped | 32,812 | 31,712 | -3% |
| boot -> app main loop | 11,189,610 insn | 10,220,202 insn | -8.7% |
| article load | 4,198,115 insn | 4,197,975 insn | -140 |
| search redraw, worst case | 1,005,062 insn | 959,991 insn | -4.5% |
| `__mulsf3` calls, whole session | 233,164 | 32,895 | -86% |

Instruction counts are exact and reproduce identically run to run. The
millisecond figures elsewhere in this file depend on the emulator's
per-instruction cycle model, which averages 1.2-1.5 cyc/instr; trust the
instruction counts further than the times.

**The biggest single win is floating point.** gcc 16 calls `__mulsf3` 7x
less often than 3.3.2 for identical source and identical output, and the
*span* is the tell: 3.3.2 calls soft-float continuously from 791 ms to
4618 ms, through idle and rendering both, while gcc 16 touches it only in a
700 ms band during typing. Across the search window that is 44.7M
instructions - 43% of the window - down to 10.2M.

**Boot is 90% SD card driver, and 45% of it is a calibrated busy-wait.**
`delay_loop` + `delay_us` come out 50 instructions apart on 5 million
between the two compilers, which is the control that says the instrument
works. Real work moved a lot: `rcvr_datablock` -61%, `wait_ready` -67%,
`File_initialise` -69%.

### What this does *not* buy

The stall distribution - the longest gaps between event-loop polls, which is
how long a tap can sit unanswered - is essentially unchanged:

```
gcc 3.3.2:  201.2  129.1  18.7  11.5  3.5  3.3 ms
gcc 16.2:   201.4  130.3  21.6  11.2  3.4  3.4 ms
```

Every stall on this device is bound by I/O or a timer, not by compute, so
the saved instructions turn into idle rather than into responsiveness. The
gains are size, headroom and battery. The one exception was a real 25%
regression in the article load, which is fixed - see below.

The worst stall in the whole session, 201 ms and identical in both builds,
is 99% `Timer_get` + `Delay_microseconds` + `Watchdog_KeepAlive`. It is a
calibrated wait; no compiler can touch it. Getting it back is a firmware
change.

### The article-load regression, and what it was

Earlier revisions of this file recorded the article load as ~11% slower and
called it an open question about `File_PowerDown` and the idle loop. That
was wrong, and both the measurement and the diagnosis were wrong.

The measurement was wrong because `-n` counts `cpu.cycles`, which the
headless idle path also advances when it fast-forwards; every "N cycles"
figure mixed real work with invented idle. The 2.17x "extra polling" was
the idle loop busy-spinning to a fixed 2 s suspend timeout - a *faster*
build completes more spins. It was never work.

The regression was real, but it was one addressing mode. Profiled to a
single phase, the article-load window is 100% `memset`, whose word loop was
five instructions where 3.3.2 emitted four, because `HAVE_POST_INCREMENT`
was never defined and GCC's auto-inc-dec pass was therefore off. Exactly
5/4. Fixed; the window is now 140 instructions from 3.3.2's on 4.2 million.

## Still open

* **Boot executes 8.7% fewer instructions but takes ~6% more wall clock**
  (230.0 -> 243.4 ms). Boot is I/O-bound so instruction count is not what
  drives it, and `spi_transmit` + `spi_receive` did go up 6.4% (3,840,964 ->
  4,087,296) while everything around them fell. Not isolated. Not guessed at.

## Known broken

* **`-O1`** - the kernel jumps to `pc=0x12` with a wild stack before printing
  anything. A real backend bug, undiagnosed. `-O2`/`-O3`/`-Os` are fine.
* **`-mno-edda32`** (the `%r15` data area) - builds and links, but the kernel
  then fails to load `init.app`. Not root-caused. Off by default; see the
  comment on `TARGET_DEFAULT_TARGET_FLAGS` for why it is also not worth
  enabling on size.
* ~~`memchr` (3.4%) and `memset` (2.9%)~~ - resolved, and the note was an
  artifact. The profiler's buckets were 64 bytes wide while `memchr`,
  `delay_us` and `delay_loop` are about 30 bytes each and adjacent, so one
  bucket covered all three and the profile named whichever came first. The
  "memchr" was the SD driver's busy-wait. Buckets are one instruction wide
  now. `memset` was real and is fixed.

## Benchmark harness notes

* Reference `.pgm` screens go **stale whenever the emulator is rebuilt**.
  Regenerate them or every comparison silently fails.
* `screen.pgm` is written into `emulator/`, and the shell cwd resets between
  commands.
* Headless runs come up powered (`bool powered = !gui` in `main.c`), so the
  power switch does not affect scripted runs.
* Bench card is a copy of `images/wrcard.img`; swap `kernel.elf` / `wiki.app`
  with `hdiutil attach -nobrowse`, `cp`, `sync`, `hdiutil detach`.
* Compare **stripped** binaries: the 3.3.2 build has symbols but no DWARF,
  ours has both, so unstripped sizes are not comparable.
* Wall-clock cycles are the right metric for *user experience* but a poor one
  for *codegen* - on this device they mostly reflect how a build interacts
  with fixed-duration I/O waits. Syscall counts and static instruction counts
  are the instruments that held up.
* `-n` is not a stopwatch: it counts `cpu.cycles`, which the headless idle
  path advances too. Read `--- work: N executed, M idle ---` instead.
* Scripted input must be anchored with `-Z`, or two builds get the tap at
  different points in their own progress and are not running the same
  interaction.
* Call counts mislead: the idle loop busy-spins to a 2 s timeout, so a
  faster build makes *more* calls to everything the poll loop touches. Use
  the stall list from `-X` for anything user-facing.
* Profile one phase (`-Y`/`-y`) and diff whole bucket dumps (`-F`). A
  whole-run profile is always the idle loop, and a top-12 hides the rest.
* See "Measuring, without fooling yourself" in `emulator/README.md`.

### Three bugs that only running could find

Everything compiled, assembled and linked with all of these present.

* **`main` was not at the entry point.** gcc 4 and later split functions into
  `.text.startup` and friends; `grifo.lds` matched only
  `build/main.o(*.text)`, so `main` - which sets up `%sp` - was not first and
  the first `push` ran with `%sp` zero.

* **Jump tables were emitted as zeroes**, so every `switch` branched to the
  same place. This port had inherited V850's 2-byte PC-relative case vectors,
  and the EPSON assembler emits 0 for a `.short` holding a difference of
  labels that appear *later* in the file - always true of a jump table. The
  original assembler has the same bug, which is why the 3.3.2 backend used
  absolute `.long` entries; this port now does too.

  This is what broke touch input: the ISR's state machine ran `1,2,3,4,5,6`
  instead of `1,2,3,4,5,0`, never reaching the case that queues an event, so
  `Event_wait` blocked forever.

### Two more binutils bugs, found by linking

Both had been sitting in the "byte-for-byte validated" port. The validation
compared `.text`, `.data` and relocations; both bugs were outside that, which
is the lesson.

* **The assembler never set the ELF header.** `e_machine` stayed 0 instead of
  `EM_SE_C33`, and `e_flags` never got the core byte (`'P'` for PE). The
  linker refuses to mix cores, so the first firmware link failed with
  "Cannot link STD object ... with PE object". The original toolchain set
  both by patching *shared* files -- reopening the finished object to poke
  byte 39 from `gas/as.c`, and a switch in `bfd/elf.c` -- neither of which
  survives into modern binutils. Now done properly via
  `elf_tc_final_processing` and `ELF_MACHINE_CODE`.
  `tools/compare-with-oracle.sh` checks both fields now.

* **`cpu-c33.c`'s `bfd_arch_info_type` initialiser was missing a field.**
  Modern BFD added a `fill` callback between `scan` and `next`, so the `next`
  pointer landed in `fill`'s slot, and the linker crashed calling it. It only
  bit on some combinations of objects, because `default_data_link_order` only
  calls `fill` when a link needs alignment padding.

### Source changes the firmware needed

Twenty-year-old code against a modern compiler. Each of these is documented
in place:

* Four cast-as-lvalue expressions in `mini-libc`'s `itoa`/`ltoa`/`utoa`/
  `ultoa` (`((unsigned)num) /= radix`), a gcc extension removed in 4.0.
* `extern inline` in `ctype.h` versus the real definitions in the `.c` files:
  a gnu89-versus-C99 difference, handled by asking for `-fgnu89-inline` when
  the compiler supports it.
* Plain `inline` definitions in headers, which under gnu89 also emit an
  external copy in every translation unit. Made `static`.
* `exit` declared `__attribute__((const))` while returning void. gcc 3.3
  ignored it -- the call survives in its output -- so dropping it changes
  nothing.
* A `packed` struct whose members are all naturally aligned 4-byte types, so
  packing changed no offset or size and only cost the struct its alignment.
* One write-only local in `wiki/lcd_buf_draw.c`, which may be a latent bug
  rather than dead code; see the comment there.

The build system needed three changes: DWARF 2 instead of `-gstabs` (gcc 16
dropped STABS, and both toolchains understand DWARF), `-fgnu89-inline` when
available, and passing the core flag to `gcc -print-libgcc-file-name` so it
returns the matching multilib.

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
4. ~~**`c33.md`**~~ - done. What remains of it is optimisation: the
   `bset`/`bclr`/`btst` bit operations, and picking short unextended
   encodings where the operand provably fits instead of always emitting the
   `x` form and letting the assembler narrow it.
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
* Once `%sp` is in a register class, `register_operand` accepts it, and a
  generic `addsi3` will claim `(set (reg sp) (plus (reg sp) N))` and then fail
  constraint checking. The dedicated `%sp` patterns must come *first* in
  `c33.md` - including a variant matching the CC-clobber parallel that the
  `addsi3` splitter produces, which is how argument pushing and alloca reach
  the stack pointer.
* An ALU immediate constraint of `i` lets a symbol through and yields
  `xadd %r5,ButtonBuffer`, which is not an instruction. Use `n`.
* GCC's virtual frame and arg pointers must report `GENERAL_REGS`, even though
  they are not real registers, because they appear in insns until elimination.
* Several C33 source files contain non-ASCII bytes, so `grep` treats them as
  binary and silently reports nothing. Use `grep -a`. This cost real time
  twice: it hid `#include "ext_remove.h"` and two live functions, and led to
  removing working code on the assumption it was dead.
* The oracle comparison checks `.text`, `.data`, relocations **and now the ELF
  header**. It did not check the header for a long time, and two real bugs
  lived there undetected through a "byte-for-byte validated" claim. When you
  add a validation, write down what it does *not* cover.
* State the method and what would falsify it *before* running a benchmark.
  Four performance claims this session were published and then withdrawn,
  every one of them an explanation offered before the number was reproduced.
* Predicting performance is worse than measuring it. Three predictions this
  port made - that the %r15 data area would close a size gap, that storing
  .bss in the file was costing boot time, and that slow article loading was
  LZMA codegen - were all wrong, and measurement said so each time.
* `size` counts read-only sections as text. The 3.3.2 build marks .rodata
  writable and ours does not, so `size` made our kernel look 22% bigger when
  it was actually smaller. Compare sections, not `size` output.
* Compiling, assembling and linking cleanly says nothing about whether the
  result runs. Three real bugs - `main` not at the entry point, jump tables
  full of zeroes, and the ELF header - survived every static check. Run it.
* A struct initialiser that compiles is not a struct initialiser that is
  correct. `bfd_arch_info_type` grew a field in the middle; the old
  positional initialiser still compiled and put a data pointer where a
  function pointer belonged.
