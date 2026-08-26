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
| GCC 16.2 backend | **runs the whole firmware**, rendering pixel-identical to gcc 3.3.2; app -16%, boot ~100 ms faster, article load ~98 ms slower |

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

### GCC - runs the whole firmware

Kernel *and* `wiki.app` build with gcc 16.2 and run in `emulator/`, booting
from `flash.rom` off a FAT32 card. Rendering is **pixel-identical** to the
gcc 3.3.2 build - search results, article view, scrolled article.

| | gcc 3.3.2 | gcc 16.2 | |
|---|---:|---:|---|
| `wiki.app` stripped | 170,976 | 144,020 | -16% |
| `kernel.elf` stripped | 35,412 | 31,956 | -10% |
| boot to usable screen | 2.435 s | **2.335 s** | ~100 ms faster |
| tap -> article rendered | **888 ms** | 986 ms | ~98 ms slower |

(Cycles at 60 MHz: boot 146.1M -> 140.1M; article 53,290k -> 59,132k after the
tap. Measured on the flash-boot path with a scripted `-K`/`-T` script and
binary search on `-n` against a freshly captured reference screen.)

**The article-load regression is real and user-visible.** An earlier commit
message claims it "was the idle loop, not code generation" - the *mechanism*
part is right, but it was then wrongly treated as meaning there is no
slowdown. A user waiting for an article waits 11% longer, whatever the cause.

## The one open question

Why the article load is 11% slower. What is established:

* Every syscall that does real work is **identical to the call** -
  `lcd_set_pixel` 15,824 both, `file_read` 135 both, `memory_allocate` 29
  both, `directory_exists` 40 both, `lcd_framebuffer_get`/`set_byte` 608 each.
* The only difference is polling: `timer_get` 282,557 -> 612,827 and
  `event_get` 96,422 -> 207,850. A clean 2.17x on both.
* That is `Event_wait`'s loop - `Event_get`, `Suspend`, repeat - spinning more
  because the loop body is faster while the I/O takes the same wall time.
* From the PC profile, `Suspend` takes its **full path** every iteration, not
  its early returns: past `File_PowerDown` and into the low-RAM suspend code.
  So each iteration powers the card down, suspends, and wakes.

What is **not** established: that the per-iteration `File_PowerDown` +
suspend + wake is what costs the 98 ms.

**Next step is one measurement, not a patch.** Count `File_PowerDown` calls
across the article load in both builds. If the ratio is ~2.17x and the
per-call cost is material, that is the answer, and the fix belongs in the
firmware's poll loop rather than in the compiler.

## Known broken

* **`-O1`** - the kernel jumps to `pc=0x12` with a wild stack before printing
  anything. A real backend bug, undiagnosed. `-O2`/`-O3`/`-Os` are fine.
* **`-mno-edda32`** (the `%r15` data area) - builds and links, but the kernel
  then fails to load `init.app`. Not root-caused. Off by default; see the
  comment on `TARGET_DEFAULT_TARGET_FLAGS` for why it is also not worth
  enabling on size.
* **`memchr` (3.4%) and `memset` (2.9%)** are in this compiler's top profile
  buckets during an article load and absent from gcc 3.3.2's. Never examined.
  Two small mini-libc routines - diffing their assembly needs no emulator.

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
