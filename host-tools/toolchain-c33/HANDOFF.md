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
| GCC 16.2 backend | **runs the whole firmware**, output byte-identical to gcc 3.3.2, and beats it on every axis measured |
| `gcc.c-torture` execute | **1676 of 1692, zero failures, at all seven of upstream's option sets**; found four wrong-code bugs the firmware could not reach |
| `gcc.c-torture` compile | **1973 of 2003 per set, zero failures, no ICEs** |
| emulator, differentially | `emulator/difftest` now runs **both** toolchains; 200 programs each, five levels, all match |
| ABI vs the 3.3.2 oracle | `tests/abi` cross-links the two compilers in all four combinations. **All agree**, 36 values, five option levels. See "The ABI - fixed, and how it was over-thought" |

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

Both toolchains now default to `-O2 -mno-long-calls`; a plain `make` gets
the numbers below.

| | gcc 3.3.2 as shipped | gcc 16.2, current defaults | |
|---|---:|---:|---|
| boot -> app main loop | 11,189,610 insn | 7,033,332 insn | **-37%** |
| article load | 4,198,115 insn | 3,148,809 insn | **-25%** |
| `wiki.app` stripped | 170,352 | 154,916 | **-9%** |
| kernel stripped | 32,812 | 36,960 | +13% |
| soft-float calls, whole session | 233,164 | **0** | - |

The search window, which is the one that moved this session and the only
place delay slots show at all, went 2600.72 ms to 2544.52 ms of modelled
time for the same 113M instructions - **1.3% from the compiler and 0.9%
from `seconds_to_ticks`**. Read the next section before quoting any of
this.

### The 8 KiB A0 boot stages

The reported gcc 16 `file-loader` overflow at `-Os` was partly a stale mixed
build: changing `OPT` does not invalidate existing objects or archives.  A
fully clean comparison still found a real code-size difference, but both
builds fit:

| clean `file-loader`, before boot-stage tuning | gcc 3.3.2 | gcc 16.2 |
|---|---:|---:|
| live A0 bytes (from `0x200`) | 6,728 | 7,424 |
| A0 headroom | 952 | 256 |

There is no missing gcc 3.3 switch: its C33 backend simply emits denser code
for these sources.  Of the 696-byte live-size gap, `mmc.o` accounts for 472
bytes and `elf32.o` for 202; `print.o` adds 82, while modern gcc makes `tff.o`
66 bytes smaller.  `-Oz` is identical to `-Os` on this backend.

The fix keeps normal SDRAM firmware at the faster `-O2`, but gives the MBR
and A0 applications their own `BOOT_OPT=-Os`.  Driver and boot objects use
function/data sections, and A0 links garbage-collect unreachable routines.
This removes the file loader's unused SD write/ioctl and formatting code
without disabling those features in firmware that uses them.  Application
entry functions have a kept first section because the hardware MBR calls
address `0x200` directly rather than consulting an ELF entry point.

After tuning, gcc 16's `file-loader` occupies 7,293 initialized bytes and
7,309 live A0 bytes, leaving 131 bytes in its flash slot and 371 bytes in A0.
The complete 64 KiB `flash.rom` builds and boots through the menu and loader
into Grifo.  A direct rebuilt-Grifo run reached `init` with 442,880 HSDMA and
442,015 IDMA transfers, zero invalid descriptor tables, and zero SD receive
overflows.  The same link layout also builds with gcc 3.3.2, and every boot
application entry remains exactly `0x200`.

The emulator now schedules SPI from the live `BPT`, `MCBR`, and `SPI_WAIT`
registers and charges the manual's minimum HSDMA/IDMA bus phases.  In matched
gcc 16 runs, a 512-byte PIO payload averages 35,395.6 MCLK cycles (589.9 us
at 60 MHz); the DMA build averages 21,358.6 (356.0 us), 39.7% less.  The
DMA-served blocks are exactly 20,472 cycles each.  The full startup still
lands on its deliberate 2000.2-ms deadline, but the interval to the final
`File_initialise` hit falls from 321.3 to 234.3 ms.  SDRAM wait states and
refresh contention remain unmodelled for both CPU and DMA, so hardware must
still calibrate the absolute numbers.

The menu is now the tightest boot component: its BSS ends at `0x1fee`, only
18 bytes below the end of A0.  It fits, but future menu growth needs an
explicit size check or a layout change.

### Read those numbers correctly

This is the part to keep hold of, because it is easy to overstate.

* **Instruction counts are exact.** They are counts of `c33_step` calls,
  deterministic, and unaffected by host load or thermals.
* **Milliseconds are a model.** `cpu.clk` comes from `cycle_cost()`, a
  per-opcode table from the manual. It charges **one cycle per load and
  models no SDRAM wait states at all**, and always charges 1 for `ext`
  though the manual says 0 or 1. So memory-heavy wins - `memset`,
  post-increment, delay slots - are the ones most likely overstated.
  In modelled time the same two headline figures are -29% and -17%.
* **The user-visible number is smaller still.** What a person feels is the
  stall - how long the UI stops answering. Tap-to-article is 129 ms -> 108 ms,
  about -16%. Everything else they touch moves by a few ms.
* **The worst stall on the device does not move at all**: 201 ms of
  `Delay_microseconds` + `Timer_get` + `Watchdog_KeepAlive`, a calibrated
  busy-wait, longer than the article load itself. No compiler can touch it.
* **"Boot" here is not the boot you sit through.** The measured window is
  kernel entry -> app main loop on the ELF path, about 230 ms. The real
  device first runs mask ROM, `mbr`, the boot menu with its timeout and
  `file-loader`; full flash boot measured ~2.4 s earlier. This work
  improved roughly a tenth of what a user waits for.

* **The article-load window is 99.95% `memset`.** 3,147,357 of 3,148,785
  instructions, profiled per function. It is a memset microbenchmark, not
  a representative article load - which is why the -25% earlier in the
  session was really "post-increment landed in `memset`", and why any
  change that does not touch `memset` measures as exactly zero there.
* **The boot instruction count above is not comparable across sessions.**
  It rose from 6,954,717 to 7,033,332 while the code got smaller and
  faster, for the reason in the next bullet. Compare boot only against a
  build measured the same day.
* **Boot instruction counts move the wrong way when code gets faster.**
  Boot is bounded by SD polling loops (`rcvr_datablock`, `wait_ready`),
  which spin until the card answers. Make each iteration cheaper and they
  spin *more* times for the same wait, so the total goes up. #4 "regressed"
  boot by 1.6% entirely this way: +89k of the +91k instructions were those
  two functions, and modelled time moved 163.0 -> 163.5 ms.
* **The card image is mutable state between runs.** The guest writes to it,
  so two runs of the same build differ unless the image is restored first.
  This produced a 0.3% phantom difference before it was noticed.

**The single highest-value measurement left is timing one fixed workload on
real hardware.** It is the only way to convert any of this into a claim
about seconds, and it would calibrate the emulator's cycle model at the
same time. It cannot be done from inside this repo.

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

## #7 - the torture suite is at 100%

Done. 1611 / 1615 / 1615 / 1615 of what is testable, zero failures at any
level, verified by a clean full run. Detail, and the compile-mode
residue, in `tests/FAILURES.md`.

Three bugs and a stale library did it. Two of the three had nothing to do
with codegen, which is the pattern worth remembering: on a port this
young, "the compiler is wrong" is usually the *last* hypothesis to be
worth testing.

### libgcc was two days stale, and `make install` hid it

Every `long long` divide in the suite returned to garbage. `__moddi3`'s
epilogue was

```
  ld.w  %sp,%r3      sp = fp
  popn  %r3
  ret
```

 - restoring `%sp` from the frame pointer but never giving back the 40
bytes of frame, so `popn` read the saved registers from the wrong end and
`ret` returned to whatever was there. That is exactly the epilogue bug
fixed at the end of last session. The compiler had been rebuilt; libgcc
had not, because libgcc's makefiles do not depend on `cc1`, and then
`make install` copied the two-day-old archive with a **fresh timestamp**,
so nothing about the tree looked stale.

The tell was that a freshly compiled function got the right epilogue and
the one inside `libgcc.a` did not. Rebuild target libraries explicitly
after any backend change:

```sh
cd .../build && rm -rf c33-epson-elf/libgcc c33-epson-elf/c33pe c33-epson-elf/c33adv \
  && rm -f configure-target-libgcc all-target-libgcc install-target-libgcc \
  && make all-target-libgcc && make install-target-libgcc
```

mini-libc needs the same treatment, and both must be rebuilt again after
an *assembler* change (the addend lives in the `.o`) and after any **ABI**
change.

### And mini-libc's archive appends rather than replaces

`mini-libc/Makefile` builds `libc.a` with `ar q` -- quick *append*. An
incremental rebuild leaves the stale members in the archive **ahead of** the
new ones, and the linker takes the first match, so the change appears to do
nothing. `libc.a` doubling from 498 KB to 1,000 KB is the tell. `make clean`
first, every time.

That is the third variant of the same trap in one session, after the
re-timestamped `libgcc` above and the ABI-stale libraries below. The general
form is worth holding on to: **when a change appears to have no effect on
this tree, suspect the build before the code.** Three times it presented as
"the compiler is wrong".

### The trampoline was still the V850's, verbatim

```
	jarl .+4,r12
	ld.w 12[r12],r19
	jmp [r12]
```

V850 mnemonics, V850 registers, V850 syntax. gas rejected all four
instructions, so every nested function failed to assemble. Nothing had
ever exercised it.

GCC 3.3.2 disabled C33 trampolines entirely (`TRAMPOLINE_SIZE 0`), although
its assembler already accepted the PE manual's documented PC-read form.  The
first replacement template missed that form and used a working but unnecessary
`call`/stack-pop sequence.  The current template follows the manual's leaf
subroutine example: `ld.w %rd,%pc` reads the following address when it is a
delayed-slot instruction, so a jump to that same address obtains the
trampoline's PC without touching the caller's stack:

```
	 0  jp.d  .+4
	 2  ld.w  %r12,%pc       %r12 = trampoline + 4
	 4  xld.w %r9,[%r12+12]  static chain at trampoline + 16
	 8  xld.w %r12,[%r12+16] function at trampoline + 20
	12  jp    %r12
	14  .short 0             alignment padding
```

The established 24-byte trampoline size and its two patched data offsets are
unchanged.  Both old and current GAS accept the documented pair, and upstream
`gcc.dg/trampoline-1.c` executes under wremu with this template.

### A relocation addend counted the PC twice, but only for static functions

`c33_elf_reloc` took its early-out - "there is an output bfd, so gas is
writing the object and the reloc is being handed on; leave it alone" -
only when the symbol was *not* a section symbol. That test is lifted from
`bfd_elf_generic_reloc`, but that function returns `bfd_reloc_continue`
in the section-symbol case and lets `bfd_install_relocation` finish,
and `bfd_install_relocation` subtracts the reloc's own address only when
`partial_inplace` is set. All 32 C33 howtos have it clear. Falling
through to the final-link code instead left the addend holding a complete
`symbol - PC`, which `ld` then relocated a second time.

gas reduces every *local* symbol to a section symbol plus addend, so the
visible rule was: a call to a global function is fine, the identical call
to a `static` function in another section goes somewhere wild. At `-O2`
GCC puts `main` in `.text.startup`, so `pr43784` called a static function
0x24 bytes short of its entry.

Note the shape of this one. The last session fixed a relocation bug in
`md_apply_fix` with the same symptom, and it was tempting to assume that
one had covered the ground. It had not: the earlier fix handled external
symbols, and the addend for local ones was being rewritten later still,
inside BFD, after `tc_gen_reloc` had already returned the right value. It
took printing the addend at three successive points to find where it
changed.

### The harness was measuring itself

Six things were being counted against the backend. `-fno-builtin` broke
every test whose point is that a builtin folds - `20021127-1` defines an
`llabs()` that calls `abort()` and passes only if GCC never emits the
call. `dg-options` was ignored, so `930529-1` ran a loop past `INT_MAX`
without `-fwrapv` and never came back. The instruction budget was 200M
when `vla-dealloc-1` honestly needs 490M. And a block of 78 "failures"
turned out to be the emulator failing to start under load, which read
exactly like a compiler regression - hence the new `NORUN` status, which
is never allowed to look like a timeout.

**The firmware is unaffected by all of it.** `wiki.app` rebuilt with the
fixed toolchain boots, types `LOVE`, and renders a screen **byte-
identical** to the one built before these changes.

### The runtime was capping the score, not the compiler

53 of the 77 tests reported "unsupported" turned out to be unsupported only
because they referenced a symbol nothing defined - `exit` under the name
mini-libc actually declares it (`__stop_progExec__`), `putchar`, `malloc`,
`setjmp`. `tests/runtime/` supplies them now, and the allocator is grifo's
own `memory.c` compiled from the firmware source rather than something
written for the occasion, so those tests put real firmware code through the
new compiler as a side effect. See `tests/FAILURES.md`.

The lesson generalises past this suite: when a harness reports a number,
check what the harness is measuring before believing it is measuring the
thing under test.

### -O3 and -Og were never run, and are clean

The suite now runs all seven option sets `c-torture.exp` uses, not four.
The two `-O3` sets are the ones that lean hardest on the delay-slot filling
and instruction-length model this port has been changing, so they were the
most likely place for something to be hiding. Nothing was.

### mini-libc's printf had two real bugs, and the compiler was right

`pr78622` and `pr79327` had been skipped for wanting `%hhd` and `%#hho`.
Both turned out to be mini-libc:

```
before                          after
[%o of 8]       -> '0'   n=2    [%o of 8]       -> '10'   n=2
[%hhd of 300]   -> '300' n=2    [%hhd of 300]   -> '44'   n=2
```

`%o` - and the BSD `%O`/`%U` spellings - never fetched an argument at all:
the prefetch before the conversion dispatch covered only `u`, `x` and `X`,
so `%o` formatted whatever `_ulong` last held **and consumed no vararg**,
desynchronising every conversion after it in the same format string.
Separately a second `h` just re-set `SHORTINT`.

Look at the `n=` column: GCC's folded return value was already correct in
both cases. `-fprintf-return-value` was right and the library was wrong,
which is exactly what those two tests exist to detect. The firmware uses
neither `%o` nor `%hh`, so nothing shipped was affected.

### What running difftest with the new compiler bought

`emulator/difftest` validated wremu only against code gcc 3.3.2 emits, and
every torture result rests on wremu. `DT_TC` now selects the toolchain.
200 programs at five levels match. The coverage measurement **contradicted
the guess that motivated the run**:

| over the same 40 programs at `-O2` | gcc 3.3.2 | gcc 16.2 |
|---|---:|---:|
| delay-slot forms | 7,081 | 3,546 |
| **post-increment `[%rb]+`** | **0** | **370** |

Delay slots were already covered - 3.3.2 fills them at a similar rate.
**Post-increment addressing was covered not at all**, because
`HAVE_POST_INCREMENT` was never defined in the old backend. It is also the
addressing mode the article-load speedup rests on, so it was the worst
thing to have had no independent check on.

## The ABI - fixed, and how it was over-thought

`tests/abi/run-abi.sh` builds the two halves of one program with the two
toolchains in all four combinations and diffs the output. This is the one
thing `gcc.c-torture` structurally cannot check: every test there is
self-contained and compiled by a single compiler, so an ABI disagreement is
invisible. It matters because the firmware links hand-written assembly and
prebuilt objects from `ROOT_IMAGE/` built by gcc 3.3.2 and not rebuildable
 - a mismatch there does not fail to link and does not crash, it silently
passes the wrong value.

**All four combinations now agree**, on 36 values, at `-O0`, `-O1`, `-O2`,
`-Os` and `-O3 -funroll-loops`. Both torture suites are clean at all seven
option sets, and `wiki.app` renders a byte-identical screen.

### The whole convention was in the tree the entire time

`host-tools/gcc-3.3.2/gcc/config/c33/c33.c` contains `function_arg`, and
`c33.h` contains `FUNCTION_ARG_ADVANCE`. Together they are about forty lines
with EPSON's own numbered comments explaining each rule. Everything that was
painstakingly reconstructed by probing - and got wrong twice - is stated
there directly.

Two things hid it. ABI.md said the ABI "must be derived by probing", which
was simply false and went unchallenged for the whole port. And `grep`
treats these files as binary because of their extended-ASCII comments, so
`grep FUNCTION_ARG c33.h` prints **nothing at all** and looks like an
answer. Use `python3` or `grep -a` on anything under `gcc-3.3.2/`.

### What the three "divergences" actually were

They were derived separately, looked unrelated, and needed three separate
fixes. In the source they are three lines of one function:

| reported as | actually |
|---|---|
| "64-bit arguments are being even-aligned" | 3.3.2 aligns too, but `BIGGEST_ALIGNMENT` is 32 so it is a **no-op for every type**. Our bug was an extra `size <= UNITS_PER_WORD &&` that sent 8-byte scalars down an `align = size` path. |
| "aggregates over 8 bytes passed by reference" | `function_arg` returns 0 for **BLKmode**, any size. Not a size rule - a 3-byte struct is BLKmode and goes on the stack, a 4-byte one is `SImode` and goes in a register. |
| "a 64-bit scalar in the last slot is split" | 3.3.2 has **no `FUNCTION_ARG_PARTIAL_NREGS` at all**. Nothing is ever split. `c33_arg_partial_bytes` returns 0, unconditionally. |

The previous attempt reverted because it tried to make `arg_partial_bytes`
model straddling - a rule the ABI does not have. The "self-contradiction
between `function_arg` and `arg_partial_bytes`" was real, and the fix was to
delete one side of it rather than reconcile the two.

`double` really is a special case, and the only one: a `DFmode` argument
that would start in the last slot goes wholly on the stack and consumes no
register, so the next argument still gets `%r9`, while a `long long` there
takes `%r9:%r10`. That is `function_arg` step 5, three lines, mirrored in
`FUNCTION_ARG_ADVANCE`.

### Two things fell out of it

* **The ICE went away.** `pr110266` ICEd in `expand_builtin_cexpi` and
  `FAILURES.md` argued it was upstream's, on the grounds that a 16-byte
  `_Complex double` is passed in memory "inherited from gcc 3.3.2". It is
  not inherited: 3.3.2 passes it in `%r6`-`%r9`. Passing it in memory was
  our bug and it was what forced expand to take the address of a
  `COMPLEX_EXPR`. The compile suite now has no ICEs at any option set.
* **`FUNCTION_ARG_REGNO_P` had to widen to `%r6`-`%r12`.** Because nothing
  is split, an argument based at `%r9` runs into `%r10` and beyond
  (a `_Complex long double` reaches `%r12`). `df` marks exactly the
  registers satisfying that macro as defined on entry, so with it stopping
  at `%r9`, `-frename-registers` took `%r10` as a scratch in a function
  whose argument was still sitting in `%r9:%r10`. `complex-7` catches it;
  it fails at `-O3 -funroll-loops` (which implies `-frename-registers`) and
  passes with `-fno-rename-registers`. Nothing to do with unrolling.

**Rebuild `libgcc` and `mini-libc` after any ABI change.** Both go stale in
a way nothing detects, and that cost a wrong diagnosis: `va-arg-19/20/21/22`
and `strncmp-1` looked like ABI regressions and were a stale libgcc.
`gcc/rebuild.sh` now does the whole thing, libgcc forced, in one command.

## The DejaGnu board file - done

Implemented in `tests/dejagnu/`. `run-dejagnu.sh` builds the freestanding C33
test runtime and invokes GCC's own `execute.exp`, `compile.exp`, or `gcc.dg`
driver. `c33-sim.exp` supplies compiler/link flags and maps the emulator
register dump to standard DejaGnu verdicts. See `tests/dejagnu/README.md` for
usage.

DejaGnu is now the sole GCC testsuite harness. The bootstrap shell runner and
its comparison-only option matrix and result summarizer were removed after
the board was qualified. Test selection, optimization matrices, directives,
and reporting now come from the upstream GCC drivers.

### Why

The bootstrap runner re-implemented DejaGnu's directive handling in `sed`,
and the audit in `tests/FAILURES.md` found **three of those mechanisms
wrong**: `dg-error` did not check the message, `undefined reference`
defaulted to UNSUPPORTED, and target selectors were ignored. The board makes
upstream's own driver evaluate the directives.

It also unlocks `gcc.dg` - **7,425 tests** in the top directory alone - which
is where the diagnostic assertions live and which the shell harness
structurally cannot run.

### Prerequisites and invocation

macOS ships `expect` 5.45 and `tclsh` but not DejaGnu.  Install it with:

```sh
brew install deja-gnu        # provides runtest
```

Then use `tests/dejagnu/run-dejagnu.sh`; it supplies the site file, board path,
compiler path and generated runtime objects.

### The three pieces

1. **`tests/dejagnu/c33-sim.exp`** - the board. `load_generic_config "sim"`,
   then `set_board_info` for compiler, `cflags` (`-mc33pe -mno-long-calls`
   plus the two include paths), and `ldflags` (`-nostdlib -nostartfiles`, the
   linker script, the four runtime objects, `libc.a`, `libgcc.a`).

2. **`${board}_load`** - invokes the emulator and returns
   `{pass|fail|unresolved} output`:

   | emulator state | verdict |
   |---|---|
   | `pc == 0x10000002` and `%r4 == 0` | pass |
   | `pc == 0x10000002` and `%r4 == 0xdead` | fail (this is `abort()`) |
   | `^fault:` in the output | fail |
   | `pc == 0x10000002`, other `%r4` | fail (non-zero exit status) |
   | no register dump at all | **unresolved**, never fail - the emulator did not run |
   | otherwise | fail (ran past the instruction limit) |

   `wremu -n 3200000000 -b 0x10000002`. The limit is a runaway detector, not
   a timeout: `vla-dealloc-1` and `pr43220` honestly need ~490M at `-O0`, and
   `pr97459-6` needs exactly 3,033,306,485 instructions. Its focused standard
   replay passes both verdicts at the 3.2B ceiling.

3. **Invocation** - `site.exp` and `run-dejagnu.sh` make
   `runtest --tool gcc --target_board=c33-sim` find the board and the
   compiler. Worth a small script next to the board file, since the paths
   (`work/install/bin`, `work/bu/install/bin`) are not standard.

### Qualification result

The board was cross-checked against the retired bootstrap runner across its
seven option sets. Every result delta was investigated; each came from the
bootstrap runner mishandling a directive, target selector, or requirement.
Focused execution, compilation-error, warning, signal, runtime-gap, and
`gcc.dg` tests then passed through DejaGnu. The standard `gcc.sum` and
`gcc.log` files are now authoritative.

An unfiltered `./run-dejagnu.sh all` completed on 2026-08-28. It recorded
145,212 expected passes, 1,836 unexpected failures, 4 unexpected successes,
921 expected failures, 1,198 unresolved tests, and 6,107 unsupported tests.
The preserved result is `tests/dejagnu/work/full-20260828-2130/` (ignored by
git). This is a broad discovery baseline, not a claim that all 1,836 raw
assertions are compiler defects.

A second unfiltered run completed on 2026-08-30 in
`tests/dejagnu/work/full-post-fixes2-20260829/`: 144,950 passes, 39 unexpected
failures, one XPASS, 924 XFAIL, 261 unresolved, and 6,166 unsupported. Those
39 failures represented 15 sources, not 39 compiler bugs.

Focused fixes after that run recover 34 failures and every unresolved family.
A declaration-only `<math.h>` exposes compile-time diagnostics without
pretending libm exists. The board reports the absent UBSan runtime, complete
gcda writer/filesystem, C++ structure-musttail probe, and hosted allocation,
formatted-output, PIC-register, and `div` prerequisites precisely. The old
C-torture driver no longer turns an explicitly unsupported compilation into
an unresolved execution. Analyzer declarations are complete, and GCC's
analyzer recognizes direct global `errno` implementations used by embedded
libcs.

Five compiler-only results remain visible because inspection shows correct
output: `ifcvt-4` assumes conditional moves C33 lacks; `pr87954` expects `*w`
while GCC prints its actual widening multiply as `w*`; `stack-usage-1` omits
C33's 16-byte saved-register block; `debug/dwarf2/inline5` has a regex that
crosses C33 `;` comments despite correct decoded DWARF; and `pr126464`
executes correctly but warns for `1e4000L` because C33 `long double` is
binary64. Upstream tests remain unchanged.

The post-fix `gcc.dg/dg.exp` replay in
`tests/dejagnu/work/gcc-dg-post-fixes-20260830/` confirms 39,358 passes, the
four non-DWARF mismatches above, 534 XFAIL, 1,037 unsupported tests, and no
unresolved result. Every execution test in that replay passes.

The focused backend/binutils results resolved after that baseline now include:

* `execute/builtins/complex-1.c` failed to link at `-O0`, `-O3 -g`, and
  `-Og -g`. C33 BFD's `c33_elf_relocate_section` rejects `sym_hashes == NULL`
  before checking whether a relocation is local; V850 delays that check until
  the global-symbol branch. Debug sections containing only local relocations
  trigger this. The check is now delayed to the global-symbol branch, the
  patched binutils were installed into the active compiler prefix, and the
  focused test passes all 16 verdicts.
* Undefined non-weak globals no longer have their placeholder zero value
  applied after ld has reported the undefined reference, so short PC-relative
  calls no longer add a bogus out-of-range warning. PC-relative relocations
  against undefined weak symbols are likewise left untouched; absolute weak
  references still resolve to zero. `visibility-22.c` now passes both focused
  verdicts without a linker warning, and the final exact-source ld suite
  remains clean at 479 passes, 13 expected failures, 28 untested, and 235
  unsupported tests.
* `gcc.dg/sibcall-{3,4,9,10}.c` failed because the C33 backend had no sibling
  call patterns. The backend now dismantles the current frame and jumps
  directly with `sjp` or `xjp`; the focused `sibcall-*.c` run records 18
  passes and zero failures.
* `gcc.dg/pr84877.c` showed that an explicitly 16-byte-aligned aggregate
  arrived on the stack at 12 modulo 16. Normal C33 frames now compensate for
  the return-address word pushed by `call`, preserving a 16-byte outgoing
  argument base; the focused test records two passes. The same model fixes
  the DWARF incoming CFA offset.
* C33's `__builtin_apply`/`__builtin_return` forwarding ABI now preserves the
  variadic stack representation while carrying typed scalar locations in a
  versioned descriptor. The complete focused apply/return and stack-alignment
  run has 107 passes and no failures.
* Precompiled headers pass all 1,254 focused verdicts with no failures.
* The backend now reports C33's documented conditional-branch cost (two
  cycles not taken, three taken) instead of GCC's generic one-instruction
  cost. `reassoc-{33,34,35,36}.c` and `update-threading.c` gain all 14
  focused compile, execution, and optimization passes.
* The normal call expanders now validate function addresses in pointer mode.
  They previously used the called byte object's `QImode`, rejected every
  symbolic target, and forced all direct calls through a register. Focused
  probes now emit `scall` and `xcall` directly; `weak/typeof-2.c` passes all 8
  assertions, the full weak-symbol driver all 93, and `tree-ssa/loop-1.c` all
  5. The post-fix core execution driver records 24,260 passes, 251 legitimate
  unsupported results, and zero failures or unresolved cases across all 1,692
  sources and their standard optimization/LTO variants.
* GCC is now configured with `--enable-initfini-array`. It emits typed,
  priority-suffixed init/fini-array sections; ld retains and consolidates
  them, orders priority 100 before 200, and supplies hidden start/end
  boundaries. Upstream `constructor-1.c` compiles and links; executing it
  remains a crt-startup responsibility outside this GCC/binutils scope.

The exact-source binutils qualification is clean: gas records 338 passes and
10 unsupported tests; the binutils utilities record 240 passes, 18 untested,
and 17 unsupported; and ld records 479 passes, 13 expected failures, 28
untested, and 235 unsupported, with zero unexpected failures in all three.
The tested tools are installed in the active GCC prefix. Objdump now selects
the Standard, Advanced, or PE opcode table from ELF `e_flags`; regression
tests cover the nine instructions the PE manual removes.

Two apparent arithmetic failures were board-budget findings. The wrapper had
silently enabled `GCC_TEST_RUN_EXPENSIVE`, and the standard `pr97459-*` cases
need as many as 3,033,306,485 emulator instructions with the current compiler
while still producing the correct answer. Expensive tests now follow GCC's
default (opt in explicitly), and the deterministic board ceiling is 3.2
billion instructions.

The second broad run exposed two macOS DejaGnu transport bugs rather than
toolchain defects. GCC's symbol scanners use Tcl's `open "| command"` directly,
so the spawn-based compatibility layer now covers read-only pipelines as well
as `exec` and `local_exec`; LTO `20081212-1` passes its `scan-symbol` check.
Also, the board's pre-link runtime classifier had opened every link input as
text. LTO `pr122515` therefore tried to read its intentional 2.88 GB archive
into Tcl 8.5 and aborted at the 32-bit string limit. The classifier now reads
only source-language files. The exact test completes with 11 passes and one
standard `memory full` unsupported link for its 320 MB extracted data object,
and cleans up its multi-gigabyte artifacts.

The post-fix complete `gcc.dg/lto/lto.exp` qualification records 1,651
expected passes, 34 unsupported tests, and no failures or unresolved cases.
The unsupported set includes explicit missing-libm and hosted-libgcov links;
unrelated undefined symbols are still failures.

The next `gcc.dg/torture` replay found and fixed three independent issues.
C33's untyped-call expander now preserves `%r5`-`%r12` across its stack-
compaction control flow and reloads them immediately before the call.  This
lets SJLJ exception expansion locate every advertised parameter load instead
of ICEing at a loop label; `harden-cfr-bret-except.c` passes all 21 assertions,
the full `harden-cfr-bret*.c` family all 204, and the neighboring focused
apply/return/stack-alignment replay all 163.  GCC's generic exception code and
the upstream tests were not changed.

The board now obtains per-test execution limits from GCC's `timeout_value`
helper rather than Expect's unscaled raw `timeout`.  Consequently upstream
`dg-timeout-factor` directives scale both the host watchdog and deterministic
instruction budget.  `inline-mem-cpy-cmp-1.c -O0`, which needs exactly
3,433,401,134 instructions and used to stop at 3.2 billion, now passes; its
complete focused matrix has 28 passes.  The DejaGnu runtime's allocator
fallback `free` is weak so a testcase may supply its own definition;
`pr59330.c` now passes all 14 compile and execution verdicts.  These are board
support changes, not firmware changes.

The torture driver was then exhaustively replayed in path-qualified
partitions. No GCC/backend failure remains. `pr47917.c -O0` reaches
mini-libc's non-conforming `snprintf` and is reported as that explicit runtime
prerequisite; every optimized and LTO variant passes. Compile-only sources use
the declaration-only test `<math.h>`, while executable libm dependencies and
their contingent `link_error_*` guards are classified only when the failed
link contains no unrelated undefined symbol. The complete IEEE replay records
1,060 passes and 66 unsupported tests with no failure or unresolved result.

The IPA driver has no demonstrated compiler failure either. External crt and
formatted-output prerequisites are reported explicitly. The complete focused
driver records 807 passes, four XFAIL, and 13 unsupported tests with no
unexpected result.

The remaining compiler-only scan mismatches are also evidence-backed there:
the ICF count includes the valid 32-bit `abs`/`labs` pair, `pr87954` uses a
different widened-multiply dump spelling, `ifcvt-4` assumes conditional-move
support C33 lacks, `stack-usage-1` omits C33's 16-byte save block from its
accepted sizes, and `inline5`'s scan omits C33's `;` comment marker from one
of its stop classes. `readelf` confirms `inline5` has the correct abstract,
inlined, and out-of-line DIE graph. None is skipped or made to pass by editing
an upstream test.

All remaining findings are tracked in `tests/DEJAGNU-TODO.md`. The active
scope is GCC and binutils correctness plus harness execution. Firmware,
mini-libc, crt startup, target services, and emulator feature work require
separate approval; their tests remain useful external-prerequisite signals.

Running the full suite also established two necessary board details: mini-libc
and runtime headers must be global board flags (freestanding `<stdint.h>` must
work), and the compiler's `bin/` directory must be on `PATH` so upstream probes
can find `c33-epson-elf-objdump` and recognize ELF weak/alias support.

### Gotchas carried over

* Runtime objects (`crt0.o`, `setjmp.o`, `runtime.o`, `memory.o`) are built
  **once per option set**, not once per test - 1692 tests otherwise rebuild
  four files apiece.
* `-w -fpermissive` is load-bearing for pre-C23 sources; dropping it will
  fail hundreds of tests for reasons that are not the backend's.
* Builtins must stay **on**. An earlier version passed `-fno-builtin` and
  silently broke every test whose point is that a builtin folds.
* `abort` is defined in `crt0.s`, not mini-libc, and that is what makes
  `%r4 == 0xdead` trustworthy. Do not let the archive version win.

## Also next, in rough order

1. **Re-measure the headline numbers.** Every figure in the performance table
   above was taken with the stale `libgcc`, and the ABI fix has since changed
   argument placement. The screens are identical, so nothing user-visible
   moved, but those numbers are stated as fact and are currently unverified.
2. **Keep the binutils prefixes merged.** The exact-source `work/bu` build is
   installed in active `work/install` and all three binutils suites are clean.
   Future rebuilds must repeat that installation or build directly into one
   prefix.
3. **Debug info beyond the line table.** The line table is now known good and
   `.debug_info` resolves correctly on unlinked objects. Nothing has run a
   real debugger, and `grifo.elf`/`wiki.app` link `--strip-all -s` so they
   carry no debug sections at all - a separate decision about link flags.
4. **Implement the skipped runtime capabilities.** The C99 libm and
   floating-point printf tests are classified accurately now, but remain
   explicit implementation debt in `tests/DEJAGNU-TODO.md`.
5. **`emulator/src/main.c` debug scaffolding** - a `WREMU_CP` env-gated block
   hard-coding `0x10052482`, left from an earlier session. Not from this work;
   worth deleting.

Both `/tmp` build trees were destroyed overnight by macOS's periodic purge
partway through this session, which is why `gcc/rebuild.sh` and the test
harnesses now default under `host-tools/toolchain-c33/work` instead. Nothing
was lost - the backend sources live in `gcc/files/` precisely so the build
tree is disposable - but do not put one back in `/tmp`.

## #6 - the article-load benchmark is one 4 MB `memset`

Not a compiler problem, and still the most valuable thing on the list.

`init_render_article` clears the whole off-screen scroll buffer on every
article:

```c
/* wiki/lcd_buf_draw.c:1013 */
if (lcd_draw_buf.screen_buf)
    memset(lcd_draw_buf.screen_buf, 0, LCD_BUF_WIDTH_BYTES * LCD_BUF_HEIGHT_PIXELS);
```

`LCD_BUF_HEIGHT_PIXELS` is `128 * 1024` and `LCD_BUF_WIDTH_BYTES` is 32 -
4,194,304 bytes, sized for a 128K-pixel-tall article. Five `memset` calls
happen in the whole article display and this one is all of the work. The
arithmetic closes exactly: 1,048,576 words at 5 cycles is 87.4 ms at
60 MHz, and the measured window is 87.46 ms. Everything this document
says about "the article load" is this one call.

Directly above it sits the version that clears only what was used,
commented out:

```c
//if(lcd_draw_buf.current_y>0)
//  memset(lcd_draw_buf.screen_buf,0,lcd_draw_buf.current_y*LCD_BUFFER_WIDTH/8);
```

An article rendering 2,000 pixels tall needs 64 KB, not 4 MB. Unknown why
it was abandoned; the obvious risk is stale pixels below `current_y`
showing through when scrolling, and whether `current_y` is even valid at
that point. It is a firmware behaviour change, not a toolchain one.

**The 20% `-Os` win in that window is real but is the most overstated
number here.** It is one loop, 1,049,088 iterations:

```
-Os                         -O2
  sub    %r9,0x1              ld.w   [%r5]+,%r10
  jrne.d                      cmp    %r5,%r9
  ld.w   [%r11]+,%r10         jrne
  = 4 cycles                  = 5 cycles
```

`-Os` counts down, so the store is independent and drops into the slot.
`-O2` compares the pointer, so the post-incrementing load feeds the
compare that feeds the branch and nothing can move. Not a general `-O2`
regression - `-O2` fills 58.0% of conditional slots against `-Os`'s
52.1%. But the saving is one cycle of *loop overhead* per word on a pure
bandwidth workload, and the model charges one cycle per store with no
SDRAM wait states. On real memory that cycle amortises against stalls.
Treat 20% as an upper bound. Not clearing the 4 MB survives contact with
real SDRAM; making the loop 1 cycle tighter may not.

### Done this session, for context

* **post-increment addressing** - `HAVE_POST_INCREMENT` was never defined,
  so auto-inc-dec never ran. Worth 25% of an article load.
* **delay slots** - `define_delay`, `%#`, and a two-byte memory alternative
  on the `Q` constraint. Worth 16% of an article load.
* **rotates** - `rl`/`rr`; a rotate had been shift/shift/or.
* **the flag-clobbering data-area address** - `add %rd,%r15` writes the
  flags and was emitted from a move pattern.
* **the entry-point omission, in four places** - see below.
* **`-mno-long-calls` and `-O2` as defaults** - measured on both toolchains.
* **honest `length` for moves** (#4) - see below; worth 0.86% of cycles,
  and it turned up a live `call.d` bug.
* **conditional-branch delay slots** (#3) - `TARGET_FLAGS_REGNUM` had been
  naming a register that does not exist, plus the same length pessimism on
  the ALU patterns. 49.7% -> 58.0% filled, against 3.3.2's 58.9%.
* **soft float** (#5) - all of it was one unfoldable constant; see below.
  Zero calls now.
* **the GCC testsuite** (#1) - run for the first time, and it found two
  wrong-code bugs the firmware had been dodging. See below.

## Known broken

Nothing outstanding. Both former entries were the same bug in different
places, and neither was in the compiler:

* ~~`-O1`~~ and ~~`-mno-edda32`~~ - fixed. See "The entry-point bug, four
  times" below.

## The optimisation matrix

Every cell builds, boots, and renders byte-identical to gcc 3.3.2 after
typing `LOVE` and tapping a result. Instruction counts are exact and
reproduce run to run; they do not depend on host load.

| | boot (insn) | article (insn) | kernel | `wiki.app` |
|---|---:|---:|---:|---:|
| gcc 3.3.2 `-Os` | 11,189,610 | 4,198,115 | 32,812 | 170,352 |
| `-Os` absolute | 9,992,025 | 3,148,852 | 31,124 | 141,088 |
| `-Os` relative | 10,064,387 | 3,148,830 | **31,008** | **141,236** |
| `-O1` absolute | 9,541,429 | 3,148,831 | 32,848 | 152,916 |
| `-O1` relative | 9,593,981 | 3,148,806 | 32,484 | 150,464 |
| `-O2` absolute | 7,033,332 | 3,148,809 | 36,960 | 154,916 |
| `-O2` relative | **7,026,873** | 3,148,796 | 36,840 | 154,156 |
| `-O3` absolute | 7,316,115 | 3,148,802 | 39,428 | 180,912 |
| `-O3` relative | 7,319,843 | **3,148,789** | 39,328 | 180,132 |

Re-measured after #4, with a fresh card image per cell. Read the boot
column with the polling-loop caveat above: it went *up* across the board
against the previous table while the code got smaller and faster.

**The data area does not matter.** Absolute against `%r15`-relative is
within 1.3% on boot, within 22 instructions on the article load, and within
0.5% on size in either direction - at `-Os` the data area is actually
*bigger*. That confirms the earlier static measurement: modern GCC already
hoists the address computation, so the data area only pays for a symbol
touched once. Keep `-medda32` (absolute) as the default; `-mno-edda32` now
works but buys nothing.

**Optimisation level does matter, and not uniformly.** `-O1` reaches the
same article figure as `-O2` at nearly `-Os` size, but boots 36% slower.
`-O3` is worse than `-O2` on boot and 26 kB bigger, past gcc 3.3.2's own
size. `-Os` used to cost 33% on the article load; it no longer does, and
its window is now the fastest of the four - see the delay-slot section.

**`-O2` is the recommendation**: fastest boot, article load within 25
instructions of the best, and `wiki.app` still 7% smaller than the
toolchain being replaced.

## What `length` has to say, and the call bug it hid

The move and extend patterns used to declare six bytes for every memory
alternative, on the reasoning that one alternative covers both the short
form and the `ext`-prefixed one. Most of those assemble to two.

`ext imm13` is a two-byte prefix, and an `x`-prefixed mnemonic lets gas
pick the narrowest encoding that works. How many bits one prefix buys
depends on whether the instruction has an immediate field of its own to
concatenate with - which is the fact that makes `%sp` special:

| form | field | 0 / 1 / 2 prefixes |
|---|---|---|
| `ld.w %rd,imm6` | signed `imm6` | 6, 19, 32 bits |
| `ld.w %rd,[%sp+imm6]` | unsigned `imm6`, **scaled** | 6, 19, 32 bits |
| `ld.w %rd,[%rb]` | none | 0, 13, 26 bits |

So a general register never reaches a nonzero displacement in two bytes
and `%sp` reaches 63 words up the frame. These came from assembling each
boundary; the manual does not say what gas will narrow.

Two things about `[%sp+imm6]` are worth keeping:

* **gas scales it for you in the `x` form.** `xld.w %r4,[%sp+16]` and
  `ld.w %r4,[%sp+4]` assemble to the same two bytes. The written operand
  is a byte offset in one and a raw field value in the other.
* **gas does not diagnose a displacement it cannot scale.**
  `xld.w %r4,[%sp+2]` assembles silently to `[%sp+0]`. The only thing
  stopping GCC emitting one is the alignment test in
  `c33_legitimate_address_p`. Do not remove it.

The good news for anyone changing this: an *under*-estimate is loud.
A short branch that cannot reach is a hard assembler error
(`operand out of range`), not a silent truncation, so the firmware
building at all is real evidence.

### The bug this exposed

Making stack references two bytes made them eligible for delay slots for
the first time, and that is when the kernel started running away during
boot. A call pushes the return address *before* its slot executes:

```
call.d  f              ; %sp -= 4, return address stored
ld.w    [%sp+0],%r4    ; ...lands on top of it
```

Every `%sp` reference in a call's slot is off by four, and `[%sp+0]`
overwrites the address just pushed - the callee returns into whatever the
slot happened to store. There were 530 in `wiki.app`.

The call patterns do not mention `%sp` in their RTL, so reorg cannot see
this for itself. There are now two `define_delay`s: calls exclude anything
mentioning `%sp`, jumps and conditional branches keep the wider rule. The
alternative - making the call patterns describe the push - is more honest
and was not taken, because it perturbs every pass that looks at a call.

### What it was worth

Filled slots up 49% in `wiki.app` and 77% in the kernel, 214 fewer `ext`
prefixes, 140 bytes smaller, all eight matrix cells byte-identical.

**0.86% of cycles for identical work** - 112.96M instructions in
2600.72 ms against 112.97M in 2578.23 ms, 1.381 -> 1.369 cycles per
instruction. That is the honest figure, and it is small. Neither headline
benchmark shows any of it, for the two reasons in "Read those numbers
correctly": boot is polling-bound and the article window is `memset`.

Also worth knowing for next time: `(eq_attr "length" "2")` does not
survive a length computed by `symbol_ref`. genattrtab substitutes the call
and then compares it against `LENGTH_2`, an enumerator that does not exist
for a numeric attribute. `(match_test "get_attr_length (insn) == 2")`
means the same thing and compiles.

## Delay slots: what finally filled them

Conditional branches sat at 49.7% filled against gcc 3.3.2's 58.9%. The
slot contents said why:

| in a conditional slot | 3.3.2 | before | after |
|---|---:|---:|---:|
| `cmp` | 305 | 77 | - |
| `add` | 110 | 11 | 67 |
| `sub` | 69 | 40 | 40 |

All of those write the flags, and reorg refuses to put a flag-writing
insn in a slot whose branch reads the flags. On this machine that
refusal is unnecessary: the branch has already decided whether it is
taken by the time the slot runs - `cond()` is evaluated at the branch in
`emulator/src/c33.c` - so the flags are dead and the slot is free to set
up the *next* compare.

`TARGET_FLAGS_REGNUM` is how a target says so, and reorg applies it only
where the branch carries a `REG_DEAD` note for the register. It had been
set to 32, inherited from the V850. `FIRST_PSEUDO_REGISTER` here is 22,
so it named nothing and the relaxation had never once applied.

That alone was worth +57 fills. `cmp` did not move, because
`cmpsi_insn` declared six bytes for its immediate alternative - a compare
against a constant was never narrow enough to be eligible. Fixing that
across the ALU patterns is what closed the gap to 58.0%.

### Three rules, not one

Do not assume the ALU immediates behave like the loads. Measured:

| | imm6 | one ext | else |
|---|---|---|---|
| `cmp` `and` `or` `xor` | signed, -32..31 | +/-2^18 | 6 |
| `add` `sub` | unsigned, 0..63 | 0..2^19-1 | 6 |
| shifts | no ext at all | - | - |

`add` and `sub` print a negative constant as the opposite operation on
its magnitude - `add %r4,-5` comes out as `xsub %r4,5` - so their width
is that of `|v|`, not of a value the unsigned field could never hold.

**Probe with `-mc33pe`.** The shift immediate is the one place the core
matters: ADV and PE take the whole `imm5` in one instruction, the
original C33 holds only 8 and gas reaches further by repeating it, so
`sll %r4,31` is one instruction on our target and four on the base core.
Every probe run for this work without the core flag got the base core's
answer.

### The branch range was wrong, and had been all along

Short branches used a 256-byte threshold. The hardware reaches 254, so
that looks like a one-step-too-generous bound and nothing more. It is
worse than that: `(pc)` in a length attribute is
`insn_current_reference_address`, which `final.cc` computes as

```c
return insn_last_address + insn_lengths[seq_uid] - align_fuzz (...);
```

 - `insn_lengths[seq_uid]` being the whole SEQUENCE, so the address
*after* the branch and its delay slot. The C33 measures the displacement
from the branch itself. Every forward branch was therefore modelled two
bytes short, or four once its slot was filled, and the threshold is now
252 to absorb it.

This was latent from before any slot was ever filled; it only became
reachable when filling slots grew the error from two bytes to four and
shrank spans onto the boundary. `qsort.c` is what finally could not
reach. It fails loudly - gas says `operand out of range` - which is the
one mercy in this area.

## Soft float: all of it was one constant

`__mulsf3` and `__fixsfsi` were the only floating point the firmware ever
reached. Nothing else - `__divsf3`, `__gtsf2`, `__floatsisf`, `__muldf3`
and the rest are linked but were never called once. The two that were ran
34,264 times each, always in pairs, inside a single 700 ms band during
typing: 4.68M instructions, 2.2% of everything executed, 137 instructions
per pair.

Every one came from `seconds_to_ticks`, which multiplies its argument by
a compile-time enum constant. There is not one variable argument in the
tree - `seconds_to_ticks(0.3)`, `seconds_to_ticks(2.1)`,
`seconds_to_ticks(LINK_ACTIVATION_TIME_THRESHOLD)`, fourteen call sites,
all literals. It was out of line in `wikilib.c` and called from four
other translation units, so none of it could fold and each call spent 137
instructions recomputing a constant.

`static inline` in the header fixes it. Soft float drops to zero calls,
worth 23.1 ms of the 2567 ms search window.

**Keep the float multiply.** The obvious follow-up is to make it integer
arithmetic, and that would change the answers: `2.1 * 60000000` is
126,000,000 exactly but **125,999,992** in single precision, so the
thresholds would shift. Inlining preserves the semantics because GCC
folds in the target's own single precision - the binary contains
`0x7829b78` and not `0x7829b80`, which is the check worth repeating if
anyone touches this.

The gcc 3.3.2 comparison in earlier notes said it called soft float 7x
more often. That was never about code quality: 3.3.2 inlined less across
the same boundary, so it made the same pointless calls more often.

## The testsuite bootstrap (historical)

This section records the initial `gcc.c-torture` bring-up under the retired
shell runner. The current testsuite uses `tests/dejagnu/`; its `gcc.sum` and
`gcc.log` are authoritative. The old numbers and diagnoses below remain useful
as history of the compiler and runtime bugs found during bootstrap.

**compile: 1964 of 2003 pass, and not one ICE.** The 39 are old sources
that modern C rejects outright - `redefinition of 'foo'` and friends -
not backend failures.

**execute, of 1692:**

| | pass | abort | timeout | fault | exit!=0 | unsup | fail |
|---|---:|---:|---:|---:|---:|---:|---:|
| `-O0` | 1553 | 5 | 23 | 0 | 0 | 77 | 34 |
| `-O1` | **1569** | 10 | 12 | 0 | 0 | 67 | 34 |
| `-O2` | 1534 | 17 | 25 | 2 | 13 | 67 | 34 |
| `-Os` | 1530 | 20 | 25 | 2 | 12 | 69 | 34 |

`unsup` is a libc this runtime does not have and `fail` is the same 34
sources the compile run rejects; neither is a backend result. Against
what is actually testable that is about 98%.

### Two wrong-code bugs, both of which the firmware dodged

**The assembler counted a symbol's own offset twice.** `md_apply_fix`
ended with an unconditional `fixp->fx_addnumber = value`, and for a
PC-relative fixup that still has a symbol, `value` is the fully resolved
target - which already includes the symbol's offset within its section.
The relocation names the symbol too, so the linker added that offset
again. The V850 this port came from has three cases there and only this
one was dropped.

It needs a same-file call *across sections*, because
`c33_pcrel_from_section` declines to resolve into another section and
leaves the relocation for the linker. So a call to the first function in
a file worked and a call to the second landed past its entry by exactly
the first one's length. One line; it took `-O2` from 1383 passes to 1525,
aborts from 76 to 17, timeouts from 108 to 33.

**The epilogue never gave the local frame back.** `expand_prologue`
establishes the frame pointer *after* carving out the locals, so it marks
the bottom of the frame - which is what `INITIAL_ELIMINATION_OFFSET`
assumes, so the frame pointer is not the thing to move.
`expand_epilogue` restored `%sp` from it and did nothing else, putting
`%sp` back where it already was:

```
    ld.w   %sp,%r3      ; gives nothing back
    popn   %r3          ; reads the saved registers 8 bytes low
    ret                 ; returns to whatever was there
```

Every function with a frame pointer and any locals returned to garbage.
`-O1` and up omit the frame pointer, so this was `-O0` only: 275 passes
became 1553.

**Both left the firmware byte-identical.** That is the point. Two
wrong-code bugs lived through the entire port because the only test was
one program built at `-O2`, and neither could be reached that way.

### If you extend the harness

Three things it has to get right, all learned the hard way:

* **Stop on a breakpoint, not on HALT.** `HALT` sets `sleeping`, not
  `halted` - grifo's suspend path waits in `HALT` for the touch
  controller, so the emulator fast-forwards through it. `.text.exit` is
  linked first so the breakpoint is always `0x10000002`.
* **`%r6` is the first argument, `%r4` is the return value.** `exit(status)`
  and a `main` that returns deliver the answer in different registers.
* **`-fpermissive -std=gnu17`.** These tests predate C23 and many call
  `exit()` without declaring it. Without it the harness reports the
  compiler's strictness as a backend failure.

What is left at `-O2` is 17 aborts, 2 faults and 13 wrong exit statuses -
a real list, in `/tmp/torture-final.txt` format, and the obvious next
thing to work through. `nest-stdar-1` and `pr43784` faulting suggests
there is still something in nested functions or varargs.

## The entry-point bug, four times

Four separate "known broken" items turned out to be one mistake repeated:
*the entry point works because of emission order*. That holds until a flag
changes which order the compiler emits in.

1. **The kernel under gcc 4+** - `main` moved to `.text.startup`, `process`
   landed at the entry address. Patched by listing `.text.startup` first in
   `grifo.lds`, which papered over the real problem.
2. **`init.app` under gcc 16** - `application.lds` had no `ENTRY`, so the
   entry defaulted to the start of `.text`; `wiki.o` holds one function and
   was fine by luck, `init.o` holds five and was not. Fixed with
   `ENTRY(grifo_main)`.
3. **`-O1`** - at `-O2` `-freorder-functions` puts `main` in
   `.text.startup`; at `-O1` it does not, and `process` was emitted first.
   Fixed with `ENTRY(main)` in `grifo.lds`.
4. **`-mno-edda32`** - reported as "the kernel fails to load `init.app`".
   It was case 2 again: the data area changes function layout, so
   `grifo_main` was not first. Fixed by the same `ENTRY`.

Every loader that starts these images jumps to `e_entry` -
`emulator/src/elf.c`, `drivers/src/elf32.c:235`, `mbr/rs232-loader.c:157`,
`grifo/src/elf32.c:231` - so naming the entry is what actually places it.

Hiding behind case 4 was a genuine compiler bug: see
`output_move_single`'s comment on why taking a symbol's address never uses
the data area. `ext hi; ext lo; add %rd,%r15` writes the condition flags,
and it was being emitted from a move pattern, so GCC scheduled it between a
compare and its branch.

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
* **Application addresses need `-M`.** `init.app` and `wiki.app` are both
  linked at 0x10040000, so an address from one application's map can fire
  while the other is running. `-M ADDR,N` holds probes, breakpoints, the
  script anchor and profile windows disarmed until ADDR has been reached N
  times; arm on the *second* `ELF32_load` and nothing can fire until
  `wiki.app` is the running program. Without it, a `-O3` measurement
  reported boot as 3,586,957 instructions against `-O2`'s 7,023,077 - a 2x
  win that was really a breakpoint firing inside `init.app`. Corrected,
  `-O3` is 3.9% *worse* than `-O2`.
* **The stall list misses a final stall that has no closing poll.** It is
  computed from gaps *between* consecutive probe hits, so a stall running to
  the end of the run is invisible. That nearly produced a claim that `-O3`
  eliminated the 201 ms wait; it does not, and the gap is visible by
  comparing `Event_get`'s last hit against `Suspend`'s.

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
4. ~~**`c33.md`**~~ - done. The C33-specific memory bit operations select
   bare and displaced forms, exclude the nonexistent `%sp` and
   post-increment encodings, and report exact 0/13/26-bit extension lengths.
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
