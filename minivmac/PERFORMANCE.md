# Mini vMac performance engineering log

This document records the WikiReader Mini vMac port's bring-up, performance
work, correctness investigations, failed experiments, and next steps. It is a
handoff document, not a claim that all numbers below are directly comparable.
The emulated Macintosh model, display path, system disk, boot phase, emulator
calibration, and profiling overhead changed during development.

Last updated: 2026-09-21. The current engine identifies itself as
`v12-regions` in profile records.

## Current state

- The port emulates a Macintosh Plus with 4 MiB of RAM, a 128 KiB Plus ROM,
  and System 7.1. A 2.6-billion-host-instruction full-system run reaches the
  Finder.
- The WikiReader shows a 240x160, one-Mac-pixel-to-one-display-pixel viewport
  into the 512x342 screen. Mini vMac's own viewport scrolling is used; there
  is no pixel doubling or resampling.
- Touch moves and clicks the Macintosh pointer. Edge motion scrolls the
  viewport, the bottom strip provides common keys and Quit, and the three
  front buttons provide click, Return, and Escape.
- The application can be installed as the card's direct boot target along
  with the matching kernel/init files. Card construction accepts Plus ROMs
  and raw or Disk Copy 4.2 floppy images.
- The default v12 JIT is correctness-tested through the Finder. It has no
  generated-code allocation errors in the long emulator run.
- Busy System 7 boot is still far from realtime. Representative complete
  windows are about 15% of realtime in the middle of boot and 19% late in
  boot. Once the guest becomes idle, physical hardware reaches about 99.2%;
  that number mainly says the scheduler can wait for the next 60.15 Hz tick.
  It does not mean that CPU-bound boot code is realtime.

The current result is therefore a usable port and a substantial execution
engine, but not a realtime System 7 boot. Physical-hardware profiling of v12
is the next required measurement; the newest v12 figures are from the
full-system WikiReader emulator.

## How to read the measurements

Profile builds close one record every ten seconds and append it to
`/minivmac/perf.log`. The important records are:

- `MINIVMACPERF`: elapsed wall time, Macintosh video ticks, tick rate, and
  percentage of the original Macintosh's realtime rate.
- `TIME`: time in the emulation core, display copy, disk I/O, synchronization,
  and profiler.
- `JIT`: translations, promotions, rebuilds, invalidations, and fallback
  counts.
- `FALLBACK`: the most frequently executed fallback opcode groups. Its keys
  are grouped (`opcode >> 3`), not necessarily one exact opcode.
- `CACHE`: active translations, generated-code use, fragmentation, reclaimed
  bytes, and allocator-integrity errors.
- `HOT`: hot guest PCs and the executed native/fallback operation totals.
- `INPUT` and `EXIT`: input flow and clean termination evidence.

The full-system emulator is valuable for repeatability, long-run correctness,
and comparing adjacent builds under the same conditions. Real WikiReader
wall-clock results remain authoritative because SDRAM wait states, LCD work,
storage latency, and instruction scheduling are hardware-specific.

Do not compare an early 128K-Mac figure with a Plus/System 7 figure as if it
were an A/B result. Within one workload, compare the same numbered profile
windows. Boot is phase-dependent: ROM checks, disk loading, decompression,
Finder startup, and idle exercise very different code.

## Progress by subsystem

### Platform bring-up and packaging

The initial port supplied Mini vMac's OS glue, monochrome screen output,
buttons, touch input, disk access, timing, shutdown, and a card layout. The
build fetches a pinned upstream revision, applies the WikiReader patch, and
generates a 4 MiB Macintosh Plus configuration. ROM and disk media remain
user-supplied and outside the repository.

Card tooling now validates Plus ROMs and both raw and Disk Copy 4.2 media. It
can construct a disposable emulator card without mounting it. The boot files
and init path were also corrected after early cards fell through to a
previous application or to a Linux console instead of starting Mini vMac.

### Display and input

The first display path scaled the Macintosh image. On a 1-bpp 240x160 panel,
that cost CPU time and turned classic gray patterns into a distracting large
checkerboard. The replacement copies a native-resolution viewport and uses
Mini vMac's existing fullscreen scrolling in two-pixel increments, preserving
the phase of gray patterns.

Writing partial guest frames directly into the visible framebuffer caused
the Welcome to Macintosh window and icon to alternate with blank rectangles.
The port now batches a complete frame through the WikiReader's framebuffer
path. Interactive pointer feedback remains immediate, while lagging emulation
does not expose an incomplete Macintosh repaint.

Touch coordinates are translated through the current viewport origin. Edge
motion invokes scrolling, and the lower control strip is kept separate from
the guest screen. This fixed the initially working display with nonfunctional
touch and scrolling.

### Persistent decode and profiling

Mini vMac's 512 KiB 68000 decode table can be persisted as
`/minivmac/m68k-v1.tbl`. Later starts validate and load it directly; missing,
incompatible, or read-only media safely use the in-memory construction path.
This removes a one-time startup cost without changing guest semantics.

Low-overhead profiling was added before treating emulator speedups as final.
It measures the core, display, disk, synchronization, JIT, cache, hot PCs,
fallback opcode shapes, input, and clean exit. The log is closed after every
window so a normal power-off preserves completed samples.

### Execution-engine generations

The progression was structural rather than a single assembly rewrite:

1. Hot CPU state, dispatch data, byte/word memory paths, and the viewport
   copier moved into the C33's zero-wait A0/IVRAM area. The large opcode table
   stayed outside so the remaining CPU state would fit.
2. The first trace engine recorded 16 guest instructions and emitted C33 code
   while retaining exact Mini vMac opcode handlers as side exits.
3. Native families grew from branches, `DBF`, `MOVEQ`, and simple moves into
   register arithmetic, logic, bit operations, `MOVEA`, `CLR`, `MOVEM`,
   displacement modes, address arithmetic, and common shifts.
4. A second-hit compile policy stopped spending generation time and cache
   space on one-shot boot paths.
5. Whole-trace cycle admission and coalesced accounting removed scheduler
   traffic between safe operations. MMIO/memory boundaries still preserve
   early-exit behavior.
6. Region compilation kept guest PC, cycle budget, and native frame live
   around proven-safe backedges. Lazy flag producers can fuse into branches.
7. Repeated invariant word reads can guard one guest address and mapping at
   entry, then keep the host pointer in a preserved register in the loop.
8. Hot immutable-ROM and proven-safe RAM regions can be re-emitted into the
   5.5 KiB fast-code window. Generations prevent transitional startup code
   from occupying it forever.
9. The SDRAM code cache grew to 16 MiB with 16,384 two-way slots. A
   boundary-tagged segregated free-list allocator now returns displaced code
   immediately and coalesces holes instead of flushing the entire cache.
10. Promoted IVRAM traces may chain while retaining native state. Chaining
    SDRAM traces was intentionally removed after it increased memory
    row-change traffic.
11. Generated memory accesses inline the one-entry guest mapping cache and
    use direct 4 MiB RAM paths where guards make that safe. MMIO and misses
    retain the exact helpers.
12. V12 added all 68000 branch conditions, byte `ADD`/`SUB`, indexed
    `(d8,An,Xn)` moves, guarded call/return/link operations, and a deliberately
    restricted two-address-register cache.

The current call engine handles `JSR`, `JMP`, `RTS`, `LINK`, and `UNLK` with a
compact direct-RAM stack path. A mapping or recorded-target mismatch exits to
the interpreter before modifying architectural state. Transfers are native
only when source and destination stay in one Mini vMac PC window.

The register cache is disabled for regions containing an interpreter fallback
or `MOVEM`. A fallback may change state not represented by the decoded opcode,
and `MOVEM` has special base-register-in-mask semantics. The restriction is
less ambitious, but it completed the full Finder boot correctly.

## Measurements

These rows capture the major checkpoints. Rows from different workload eras
are historical context, not direct A/B comparisons.

| Workload and engine | Result | Interpretation |
| --- | ---: | --- |
| Early interpreter control, pre-Plus workload | 9.52 ticks/model second | Baseline with the same A0 CPU and memory work but no JIT |
| V7 trace engine, pre-Plus workload | 24.57 ticks/model second | First large translator gain |
| V8, pre-Plus workload | 46.52 ticks/model second | Scheduler/region overhaul; about 2.45 modeled C33 cycles per host instruction |
| V9, 600M-host-instruction test | 48.45 ticks/model second | 1183 video ticks; 4.1% over v8 and about 80.6% of original-Mac tick rate in this older workload |
| Native-viewport physical idle run | 59.84 ticks/second | Realtime idle with display work reduced to 8.7%; not a busy-boot result |
| V10, physical Plus/System 7, windows 5-12 | 17.43% realtime | Current workload established that busy boot remained the real problem |
| V10, physical Plus/System 7 idle | 99.2% realtime | Synchronization/waiting dominates once the guest is idle |
| V11, emulator Plus/System 7, windows 5-12 | 15.57% realtime | Baseline immediately before v12 coverage work |
| V11, emulator Plus/System 7, windows 13-18 | 18.90% realtime | Late-boot comparison baseline |
| V12 coverage-only, windows 5-12 | 15.28% realtime | Large fallback reduction did not automatically improve whole-boot throughput |
| V12 coverage-only, windows 13-17 | 18.70% realtime | Late-boot comparison without native control transfers |
| V12 final, windows 5-12 | 15.21% realtime | Essentially flat versus the adjacent baselines |
| V12 final, windows 13-17 | 19.14% realtime | 2.4% over coverage-only and 1.3% over v11 late boot |

One representative early hot window improved from 13.2% to 16.9% realtime
when full condition, byte arithmetic, and indexed-address coverage reduced its
fallback share from about 36% to 4.5%. That is a meaningful local result, but
it did not persist across every phase of the full boot.

In the final late window, guarded control flow reduced executed fallbacks from
450,914 of 1,092,655 operations (41.3%) in the coverage control to 344,187 of
1,127,805 (30.5%). The final cache had 10,738,080 bytes in use, 6,039,136 free,
7,314,000 cumulatively reclaimed, and zero allocation errors. The run reached
the Finder after 2.6 billion host instructions, with a valid generated-code
dispatcher PC and no watchdog failure.

The earlier reclaiming-allocator stress result likewise reached Finder with
no whole-cache flush: about 7.49 MiB remained live, 8.51 MiB free, and 5.46 MiB
had been reclaimed. The previous bump allocator had reached about 15.9 MiB and
discarded all translations during the same broad boot workload.

## What the investigation established

The largest cost is not simply the 68000 arithmetic itself. It is the boundary
around that arithmetic: opcode dispatch, lazy flags, guest-to-host address
translation, helper calls, cycle accounting, and fetching generated code and
state from wait-stated SDRAM. Eliminating a fallback only wins if the emitted
replacement is compact and avoids enough of those boundaries.

This explains why a 60 MHz PowerPC 601 could run a mature 68K emulator much
better than a 60 MHz C33 here. Clock rate alone hides major differences:
caches, memory bandwidth and latency, pipeline width, register count, branch
machinery, compiler/assembly quality, and the amount of guest state that can
stay resident. The WikiReader also fetches much of its generated code and data
through constrained SDRAM, so a larger native expansion can lose despite
executing fewer interpreter handlers.

It also explains the apparent idle/boot contradiction. At idle, Mini vMac
does little work and deliberately synchronizes to the Macintosh tick rate.
During boot, System 7 continuously exercises CPU, disk, memory translation,
and large changing code paths. A 99.2% idle display is evidence of correct
pacing and sufficient idle headroom, not evidence of 99.2% CPU throughput.

## Dead ends and correctness failures

These experiments are worth retaining because several produced attractive
numbers that were not real speedups.

### Display scaling and live partial frames

Pixel doubling/scaling was both expensive and visually wrong for a 1-bpp
panel. Directly exposing the partially updated Macintosh framebuffer then
caused blank rectangles and flashing borders/icons. Native 1:1 viewport copy,
two-pixel scrolling, and complete-frame presentation were the durable fixes.

### Generic condition handling

An early generic all-condition route was slower because it still crossed a
helper boundary. The useful version specializes the known lazy producer and
emits a direct C33 comparison/branch, retaining exact materialization only for
rare representations.

### SDRAM trace chaining

Keeping native state across every SDRAM trace sounded structurally superior,
but it increased row-change traffic enough to lose performance. Only promoted
fast-memory traces chain today.

### Bump-only generated-code storage

The bump allocator filled approximately 15.9 MiB during boot and flushed all
translations. Increasing capacity delayed the cliff but did not solve it.
Immediate reclamation plus coalescing removed the structural failure.

### Direct-RAM paths by themselves

A short sample suggested roughly 1.5% lower modeled cycles per instruction,
but the complete windows measured 15.24% versus the 15.57% v11 baseline. The
paths remain useful infrastructure and enable compact call frames; they were
not a standalone headline gain.

### Unrestricted guest address-register caching

Caching guest A registers in preserved C33 registers initially survived short
runs, then stalled late boot in a ROM loop near guest PC `0x4013fa` (opcode
`66f6`), with fallback group `0838` repeating at about 11.6%. Bisection
exonerated the newly added opcode families and identified stale state across
fallback/`MOVEM` boundaries. Excluding such regions restored a complete Finder
boot.

### Immediate arithmetic

The immediate-arithmetic experiment reported spectacular 57-98% windows, but
the viewport went blank, disk traffic was roughly halved, and the guest had
fallen into false idle. It is disabled by default. These measurements are
invalid and must not appear in performance comparisons until per-opcode
differential tests establish correct results and flags.

### `CMP`/`TST` expansion

The broad `CMP`/`TST` experiment caused a real guest reset. It is disabled by
default and should return one encoding/addressing family at a time under
differential testing.

### First native call engine

The initial control-transfer implementation routed stack accesses through
generic mapping machinery. It grew generated code by about 12% and erased its
late-boot benefit. The retained implementation uses a compact guarded direct-
RAM stack path.

### Cross-window control transfers

An experiment assumed that addresses in the same ROM backing had a linear
host-pointer relationship across Mini vMac PC windows. A 500-million-
instruction run eventually jumped to invalid host PC `0x00001234`. The
assumption is false: a guest address and backing object do not by themselves
prove that the current host PC mapping extends to the target. V12 therefore
restricts native control transfers to the recorded PC window.

### Short-run success

Full condition, byte, and indexed coverage first looked about 34% faster in a
short interval, then appeared to stick late in boot. The eventual root cause
was the unrestricted foundation register cache, not those opcode families.
This reinforced two rules: bisect orthogonal features, and require a complete
Finder boot before accepting a performance result.

## Current safe build configuration

The defaults in `Makefile` are the accepted configuration:

| Setting | Default | Reason |
| --- | ---: | --- |
| `JIT` | `1` | Enable the C33 translator |
| `PROFILE` | `0` | Keep production overhead out; use `1` for measurement |
| `JIT_NATIVE_MASK` | `0x1fff` | Enable all currently accepted native families, including guarded calls |
| `JIT_INLINE_MEMORY_MASK` | `0x23` | Accepted inline memory paths |
| `JIT_SLOTS` / `JIT_WAYS` | `16384` / `2` | Large two-way translation working set |
| `JIT_CODE_BYTES` | `16777216` | 16 MiB reclaiming generated-code arena |
| `JIT_DIRECT_RAM` | `1` | Guarded direct 4 MiB RAM access |
| `JIT_IMMEDIATE_ARITH` | `0` | Disabled after false-idle correctness failure |
| `JIT_CMP_TST` | `0` | Disabled after guest reset |
| `JIT_BYTE_ARITH` | `1` | Validated byte `ADD`/`SUB` coverage |
| `JIT_AINDEX` | `1` | Validated indexed move coverage |
| `JIT_REG_CACHE` | `1` | Restricted, validated two-address-register cache |

Always clean before changing these command-line settings. The generated build
does not express every command-line variable as a dependency, so reusing old
objects can silently invalidate a comparison.

## Prioritized next steps

### 1. Measure v12 on physical hardware

Install a clean `PROFILE=1` build and matching boot files, boot through Finder,
interact with the viewport, quit normally, then analyze the card's
`/minivmac/perf.log`. Compare the same boot phases with v10 hardware records,
especially `TIME`, executed fallback share, cache occupancy, disk time, and
whether the emulator's late-window improvement survives real SDRAM behavior.

This is the first step because it decides whether the next target is still the
execution boundary, generated-code fetch, display, or storage on the device.

### 2. Add a correct cross-window guest-PC mapping mechanism

The largest structural control-flow opportunity is not another guessed host
pointer. Store or derive a stable logical guest target, then resolve it through
Mini vMac's authoritative PC mapping before entering generated code. A small
guest-PC-to-host page/TLB layer could make common cross-window `JSR`, `JMP`,
and returns safe while avoiding the full interpreter round trip.

Late v12 fallback groups included `4eb8` (22,013), `b0a8` (19,873), `4ef8`
(17,564), `0c40` (14,965), `4ea8` (13,725), `2078` (10,794), `4a40` (8,855),
and `3440` (7,884). Because these are grouped keys, decode the exact hot
opcodes and guest PCs before implementing a family. The `4e*` groups strongly
support revisiting control transfers with correct mapping semantics.

### 3. Restore rejected families under differential tests

Build a host-side or dual-engine opcode harness that runs one instruction with
identical registers, memory, flags, and PC through the exact Mini vMac handler
and the emitted implementation, then compares complete architectural state.
Use it to restore immediate arithmetic and `CMP`/`TST` one encoding and
addressing mode at a time. Include aliasing, sign extension, X/C distinctions,
address errors, MMIO exits, and PC-window edges.

### 4. Improve code density, not just native coverage

The flat middle windows show that fallback reduction can be cancelled by
larger SDRAM instruction streams. Split cold side exits into shared
trampolines, specialize common encodings, avoid repeated constants/guards,
and measure bytes emitted per executed guest operation. Promote based on
estimated saved boundary cost per fast-memory byte, rather than hit count
alone.

### 5. Build a real guest-register allocator

The current two-A-register cache proves the concept but deliberately rejects
many regions. A broader allocator needs explicit live-in/live-out sets, dirty
bits, helper clobber descriptions, precise `MOVEM` semantics, and side-exit
writeback. That can keep more guest data and addresses resident without
repeating the stale-state failure.

### 6. Reduce memory-translation and dispatcher boundaries

Extend guarded mapping only where stable page-generation information makes it
correct. Consider a compact guest page/TLB cache shared by loads, stores, and
PC mapping. Keep the A0 dispatcher and common miss paths small, schedule C33
loads around known latency, and validate alignment/branch layout on physical
hardware.

### 7. Separate CPU and storage limits during boot

Use `TIME` records and disk counters to distinguish execution stalls from
floppy/image latency. Decode-table persistence is already complete; further
I/O work is worthwhile only if hardware windows identify disk time as a major
fraction. A CPU optimization cannot accelerate time spent waiting for card
I/O.

### 8. Automate correctness milestones

Keep long-run checks for first video, Happy Mac, Welcome to Macintosh, Finder,
screen activity, disk progress, generated-code PC range, cache integrity,
clean exit, and input. Save adjacent-build profile summaries automatically.
This would catch false idle, reset loops, blank frames, and invalid host PCs
before a misleading average is accepted.

Reaching realtime busy boot may ultimately require most of these structural
changes together: safe cross-window native control flow, denser code, broader
register residence, and fewer mapping/dispatch boundaries. If exact emulated
timing is not mandatory, an optional boot-time turbo mode could improve user
experience, but that is a semantic tradeoff and should remain separate from
claims about execution-engine performance.

## Reproducing and validating

Build production and profile variants from clean trees:

```sh
make -C minivmac clean
make -C minivmac

make -C minivmac clean
make -C minivmac PROFILE=1
```

Build the interpreter control separately:

```sh
make -C minivmac clean
make -C minivmac JIT=0
```

Construct and run a disposable full-system emulator card:

```sh
python3 samo-lib/mbr/make-flash.py build/minivmac/flash.rom
python3 minivmac/make-card.py build/minivmac/card.img \
  /path/to/MacPlus.ROM /path/to/System.dsk
emulator/wremu -g -e build/minivmac/flash.rom \
  -c build/minivmac/card.img
```

For every candidate intended as a new default, require all of the following:

- a clean build with the intended toggles;
- no watchdog, alignment, extension-prefix, allocator, or cache-integrity
  error;
- normal disk progress and nonblank, nonflashing screen updates;
- a valid interpreter, dispatcher, or generated-code host PC at termination;
- a complete Finder boot, not merely an attractive short window;
- working pointer, click, viewport scrolling, controls, and clean Quit;
- matched-window comparison on the emulator followed by physical-hardware
  confirmation.

## Relevant files

- `Makefile`: feature switches and accepted defaults.
- `cfg/WRM68KJIT.h`: recorder, native C33 emitter, regions, register cache,
  memory guards, code cache, allocator, and profiling counters.
- `minivmac.c`: WikiReader platform glue, display/input, decode persistence,
  logs, timing, disk handling, and app lifecycle.
- `patches/wikireader-fast-m68k.patch`: upstream integration hooks and hot
  interpreter changes.
- `make-card.py`: deterministic emulator-card construction and media checks.
- `fetch.sh`: pinned-source fetch and patch/config generation.
- `README.md`: user-facing build, install, controls, and architecture summary.
