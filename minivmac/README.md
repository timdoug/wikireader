# Mini vMac on WikiReader

This port runs a maxed-out 4 MiB Macintosh Plus with Mini vMac on the
WikiReader's Epson C33: 68000, monochrome display, no sound, and one or more
floppy images. The upstream
source is fetched at the revision in `revision`; Apple ROM and system images
are not part of this repository.

See [`PERFORMANCE.md`](PERFORMANCE.md) for the performance-engineering log,
including comparable measurements, failed experiments, current safe defaults,
and the prioritized work still needed for realtime boot performance.

## Build

From the repository root:

```sh
make minivmac
```

The first build clones the pinned Mini vMac source, applies the small
WikiReader performance patch, and generates its 4 MiB Macintosh Plus configuration under
`minivmac/work/`. Outputs are `minivmac/minivmac.app` and
`minivmac/minivmac.ico`. `make -C minivmac realclean` also removes the fetched
tree; ordinary `clean` keeps it.

## Card layout

Copy the application and icon to the boot volume and add this launcher entry:

```text
minivmac.ico : minivmac.app
```

Create `/minivmac` on that volume containing:

```text
MacPlus.ROM     128 KiB Macintosh Plus ROM
disk1.dsk       first raw or Disk Copy 4.2 floppy image
disk2.dsk       optional second image (up to disk6.dsk)
```

The ROM must be supplied by the user. Plus ROM revisions 1, 2, and 3 are
accepted. The port supports raw and Disk Copy 4.2 images at 400K, 800K, and
1.44MB; sector tags are preserved when present. Disk images are opened
writable when the card permits it and read-only otherwise. Ejecting a disk
flushes it.

## Display and controls

The upper 240x160 pixels are a native-resolution viewport into the
Macintosh's 512x342 monochrome screen. Moving the pointer into the viewport's
edge band invokes Mini vMac's fullscreen auto-scroll path, which pans in
two-pixel steps to keep classic Macintosh gray patterns stable. There is no
resampling: every displayed bit is one Macintosh pixel. A touch maps through
the current viewport, moves the Mac pointer, and holds its button until
release. The bottom strip supplies Command, Shift, Escape, Return, Space, and
Quit. The front buttons are mouse click, Return, and Escape respectively.

This is an initial bring-up port. It has no sound or full on-screen keyboard.
The viewport copier, 68000 dispatch loop, live CPU state, and byte/word read
paths use the C33's zero-wait on-chip RAM. The 512 KiB opcode table is
allocated separately so that the remaining 232-byte CPU state fits there. In
a repeatable
full-system emulator boot sample this now raises Macintosh video ticks from
6.24 to 46.52 per modeled second, about 646% over the initial port. It reaches
the early system-disk boot screen with no alignment fault or watchdog timeout.

`cfg/WRM68KJIT.h` contains the 68000-to-C33 trace translator enabled by the
default build. It emits native C33 for branches, `DBF`, `MOVEQ`, byte/word/long
moves with common no-extension addressing modes, register arithmetic, and the
boot workload's hot `BTST.B` forms, with exact Mini vMac handlers as fallback.
Its span-based `MOVEM.L` engine maps each stack-frame transfer once, then
unrolls the endian-correct register loads or stores; common register/immediate
`ADDA` and `SUBA` forms also stay inside translated traces.
It also handles `(d16,An)` moves, quick address/word arithmetic, and the hot
register `ASR.W` path natively. Register and immediate-to-register forms of
`AND`, `OR`, and `EOR`, register `NOT`, plus quick long data-register
arithmetic now avoid the legacy decoder as well. Common `MOVEA.W`/`MOVEA.L`
sources and `CLR.B`/`CLR.W`/`CLR.L` destinations are native too; `MOVEA.W`
retains its required sign extension and neither `MOVEA` form changes the
68000 condition codes.
Common byte/word reads and long writes inline the core's one-entry guest-memory
translation cache in generated C33 code. Cache misses still call the exact
Mini vMac helpers, which refresh the mapping and preserve MMIO behavior.
Traces contain 16 guest instructions. A new trace is recorded and interpreted
on its first encounter, then emitted only if execution returns to it. This
second-hit policy avoids spending native-code generation time and cache space
on one-shot startup paths.
Native `BRA` and common `MOVE`/`BTST` paths advance the guest PC with
short deltas, avoiding repeated 32-bit address materialization.
The second-generation scheduler admits a trace only when its complete fixed
68000 cycle cost fits in the current slice. It then coalesces cycle accounting
and retains scheduler exits only at fallback and guest-memory boundaries,
where MMIO can request an early return.
The third-generation region path recognizes recorded backedges and keeps the
guest PC, cycle budget, and native stack frame live across loop iterations.
Regions are activated only after IVRAM promotion. Immutable ROM loops are
always eligible; validated RAM loops are eligible when dataflow proves that
every operation is register/control-only, so the region cannot modify its own
code or touch MMIO before the next validation boundary. Short backedges use a
single C33 branch instead of a three-instruction extended jump.

Region dataflow also tracks lazy condition-code producers. When a branch is
fed by a proven `TST.L`-equivalent result from `MOVE`, `MOVEQ`, `TST`, or a
logical operation in the same trace, the emitter fuses them into a direct C33
comparison and branch. The condition engine now also handles every 68000
`Bcc`/`DBcc` condition natively, evaluating common lazy `TST`, compare,
subtract, add, and negate forms without entering an opcode handler. Exact Mini
vMac flag materialization remains the fallback for rare lazy representations.
This keeps the generic handler, lazy-flag dispatch table, and condition
callback off the recorded path without weakening condition-code correctness.
The v9 region allocator also recognizes repeated word reads through one
unchanged guest address register and displacement. It resolves that invariant
effective address once, guards the guest-register value and memory mapping at
dispatch, and keeps the resulting host pointer live in a preserved C33
register across native backedges. The loop body can then load the word
directly, avoiding repeated guest-register loads, address arithmetic, MATC
checks, map construction, and memory-boundary cycle exits. If either the
register or mapping changes, the block is rejected and rebuilt before it can
execute with a stale pointer.
Translations start in SDRAM; blocks that execute 64 times are re-emitted into
the unused 5.5 KiB IVRAM window. When a displaced trace remains hot, the cache
starts a new promotion generation so transitional boot traces do not remain
there forever. Trace metadata omits an unused mapping pointer and pads each
record to exactly 512 bytes, turning the scheduler's direct-map address
calculation from two multiplies into one shift while still using less memory.
The SDRAM cache retains 16 MiB of generated code in 16,384 two-way
set-associative trace slots. This uses the Plus port's 32 MiB RAM headroom to
keep substantially more of System 7's startup working set resident and avoid
pathological direct-map collisions. Generated code uses a boundary-tagged,
segregated free-list allocator: replacing or invalidating a trace immediately
returns its SDRAM code, and adjacent holes coalesce instead of forcing the
whole JIT cache to be discarded. Allocation normally emits once into a
conservative block and returns its unused tail; only a fragmented arena needs
a size-only emission pass. Promoted immutable-ROM traces can now tail-chain
to another promoted trace while keeping the native stack frame, guest PC, and
cycle count live. Slot identity and cycle admission are checked at every link;
SDRAM traces deliberately retain dispatcher returns because chaining them
increased row-change traffic. The byte-memory mapping fast path also runs from
A0 RAM. Zero-offset generated loads and stores also omit their redundant C33
extension prefix. A deterministic 600-million-instruction boot test reaches
1183 video ticks without alignment, watchdog, guest-state, DMA, or
extension-prefix faults. Its longer steady interval averages about 48.45 ticks
per modeled second, 4.1% above v8 and 97.2% above the v7 trace engine's 24.57
result. This is about 80.6% of the original Macintosh's 60.15 Hz pace, leaving
a 1.24x gap. The v8 baseline reached 1016 ticks and 46.52 ticks per modeled
second; its SDRAM wait cycles were about 438 million and modeled C33 cost was
2.45 cycles per instruction.
With the same A0 CPU state and memory routines but `JIT=0`, the
300-million-instruction control run reaches 9.52 ticks per modeled second. The
v9 region build reaches 36.36 ticks per modeled second over the corresponding
startup interval, about 282% above the interpreter.

A 2.6-billion-C33-instruction Macintosh Plus/System 7.1 boot stress reaches
the Finder without a generated-code cache flush or allocator error. At the
end, 7.49 MiB of the 16 MiB arena is live, 8.51 MiB remains free, and 5.46 MiB
has already been reclaimed from displaced traces. The previous bump allocator
reached about 15.9 MiB and discarded every translation earlier in the same
workload. The reclaiming allocator therefore removes a major boot-time cliff
without materially changing early-window throughput.

The v12 region engine adds three broader native families: byte `ADD`/`SUB`,
indexed `(d8,An,Xn)` moves, and guarded `JSR`, `JMP`, `RTS`, `LINK`, and
`UNLK`. Call-frame longwords use a compact direct 4 MiB RAM path; a mapping or
recorded-target mismatch exits through the exact interpreter before changing
architectural state. Control transfers are deliberately native only while
their source and recorded destination remain in one Mini vMac PC window.
Cross-window calls stay interpreted because the core may change the host PC
mapping even when both guest addresses name ROM.

Two preserved C33 registers may hold the hottest guest address registers in a
region. Regions containing an interpreter fallback or `MOVEM` are excluded:
fallback handlers can change registers not described by the decoded opcode,
and `MOVEM` has special base-register-in-mask semantics. This restriction was
validated by a complete Finder boot and avoids stale guest-register state.
The same run completed with zero JIT allocation errors. In a representative
early boot window, the new branch/byte/index coverage reduced the fallback
share from about 36% to 4.5% and raised emulated speed from 13.2% to 16.9% of
realtime. In the late Finder phase, guarded control flow reduced fallback
execution from 41.3% to 30.5%; the five final windows averaged 19.14% realtime
versus 18.70% without it. These are full-system emulator results and still
need physical-hardware confirmation.

A 290-second physical-hardware profile of the scaled-display build produced
29 complete ten-second windows and a clean exit record. After its two startup
windows, it sustained 59.64--59.75 Macintosh video ticks per second, or
99.1--99.3% of the original Macintosh tick rate. There were no JIT flushes,
specialization invalidations, disk stalls, watchdog failures, or log errors.
The stable windows spent about 37.1% of wall time in the emulation core, 19.0%
in the old scaler, 43.1% synchronized waiting for the next Macintosh tick,
and 0.9% in profiling. A full-system validation of the replacement native
viewport also reached 59.84 ticks per second while reducing display work to
8.7%, leaving substantially more realtime headroom and eliminating the gray
pattern aliasing.

Busy boot now has two additional startup optimizations. The 512 KiB generated
opcode-decode table is persisted as `/minivmac/m68k-v1.tbl`; a valid cache is
loaded directly on later starts, while a missing or incompatible cache is
built and written once. Read-only media safely retains the original in-memory
construction path. Preinstalling the table avoids both table construction and
the one-time write penalty on the first hardware boot.

In the calibrated full-system emulator, the cached second-hit engine with the
specialized zero-condition and logical-operation paths moves first video from
about 2.72 to 2.04 modeled seconds. Its busy interval rises from 40.0 to 45.9
video ticks per modeled second, about 14.7%. A 600-million-instruction
validation produced 1629 video ticks, averaged 54.3 Hz from first video
through the later idle interval, and completed without watchdog, alignment,
or execution faults. These are emulator measurements; a new physical boot
profile is still required to quantify the wall-clock improvement on the
WikiReader itself. Those earlier figures predate the 4 MiB Macintosh Plus and
System 7.1 workload and should not be compared directly with its startup rate.

Build the old interpreter separately for comparison so stale JIT objects are
not reused:

```sh
make -C minivmac clean
make -C minivmac JIT=0
```

For a low-overhead physical-hardware profile, clean and build with
`PROFILE=1`. Every ten real seconds the bottom strip shows Macintosh video
ticks per second, percent of real-time Macintosh speed, and the time shares
for the CPU/core, LCD scaler, disk transfers, scheduler synchronization, and
the profiler itself. The following lines report JIT block builds, promotions,
evictions, fallback instruction shapes compiled, cache occupancy, and
generated-code use. `CACHE` also records the largest free extent, free-chunk
count, peak live use, cumulatively reclaimed bytes, total free bytes, and an
allocator-integrity error count. A `HOT` record weights fallback coverage by
actually executed blocks instead of counting cold compiled code equally. The
same measurements, expanded with JIT totals and invalidation reasons, are
appended to `/minivmac/perf.log`; each window is closed immediately so it
survives a normal power-off.

```sh
make -C minivmac clean
make -C minivmac PROFILE=1
```

For the full-system emulator, create a disposable card without mounting it:

```sh
python3 samo-lib/mbr/make-flash.py build/minivmac/flash.rom
python3 minivmac/make-card.py build/minivmac/card.img \
  /path/to/MacPlus.ROM /path/to/System.dsk
emulator/wremu -g -e build/minivmac/flash.rom \
  -c build/minivmac/card.img
```
