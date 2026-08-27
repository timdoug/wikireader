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
| GCC 16.2 backend | **runs the whole firmware**, output byte-identical to gcc 3.3.2, and beats it on every axis measured: 38% fewer instructions to boot, 25% fewer on an article load, app 9% smaller |

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
| boot -> app main loop | 11,189,610 insn | 6,954,717 insn | **-38%** |
| article load | 4,198,115 insn | 3,148,782 insn | **-25%** |
| `wiki.app` stripped | 170,352 | 155,368 | **-9%** |
| kernel stripped | 32,812 | 37,032 | +13% |
| `__mulsf3` calls, whole session | 233,164 | 32,895 | -86% |

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

## The work queue, in the order agreed

**#6 - one unfilled slot in `memset` costs 20% of the article window.**
The best-characterised win left, and it is not a backend bug.

`-Os` runs that window in 69.97 ms where every other level takes 87.46 ms,
at the same instruction count. It is one loop, 1,049,088 iterations:

```
-Os                         -O2
  sub    %r9,0x1              ld.w   [%r5]+,%r10
  jrne.d                      cmp    %r5,%r9
  ld.w   [%r11]+,%r10         jrne
  = 1 + 2 + 1 = 4 cycles      = 1 + 1 + 3 = 5 cycles
```

1,049,088 cycles at 60 MHz is 17.5 ms, which is exactly the 87.46 - 69.97
gap. `-Os` counts down and compares against the counter, so the store is
independent and drops into the slot. `-O2` compares the pointer against
its limit, so the post-incrementing load feeds the compare that feeds the
branch, and nothing can move.

This is not a general `-O2` regression - `-O2` fills 58.0% of conditional
slots against `-Os`'s 52.1%. It is one induction-variable choice in one
hot loop. Worth knowing before chasing it: the article window is 99.95%
`memset`, so this is 20% of a benchmark that is itself unrepresentative.

**#5 - soft float.** 32,895 `__mulsf3` calls survive, down from gcc 3.3.2's
233,164. The span is the tell: 3.3.2 called soft float continuously from
791 ms to 4618 ms, through idle and rendering both; gcc 16 touches it only
in a 700 ms band during typing. Find what still computes in float and
whether it needs to - `seconds_to_ticks` is one known caller.

**#1 - the GCC testsuite has never been run.** The big one, and it is not
performance. Everything here is validated by "one firmware renders
identically", which is a single program exercising a fraction of the
language. `gcc.c-torture` and `gcc.dg` against a simulator target would be
orders of magnitude more coverage, and it is the difference between "works
for the WikiReader" and "is a C compiler". The ordering agreed puts this
last. #4 has now gone in without it, on the strength of eight matrix cells
rendering byte-identical - which did catch the `call.d` bug, but only
because that one happened to be fatal during boot rather than subtly
wrong somewhere the test does not look.

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
| `-Os` absolute | 10,023,943 | 3,148,854 | 31,124 | 141,192 |
| `-Os` relative | 10,029,985 | 3,148,830 | **31,008** | **141,308** |
| `-O1` absolute | 9,557,074 | 3,148,832 | 32,848 | 152,984 |
| `-O1` relative | 9,598,701 | 3,148,808 | 32,484 | 150,540 |
| `-O2` absolute | 7,043,744 | 3,148,811 | 36,960 | 155,020 |
| `-O2` relative | **7,030,467** | 3,148,800 | 36,840 | 154,268 |
| `-O3` absolute | 7,328,463 | 3,148,802 | 39,428 | 181,000 |
| `-O3` relative | 7,322,374 | **3,148,791** | 39,328 | 180,232 |

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
