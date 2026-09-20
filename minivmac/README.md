# Mini vMac on WikiReader

This port runs a 512 KiB Macintosh 128K with Mini vMac on the WikiReader's
Epson C33. It is deliberately the smallest useful configuration: 68000,
monochrome display, no sound, and one or more raw floppy images. The upstream
source is fetched at the revision in `revision`; Apple ROM and system images
are not part of this repository.

## Build

From the repository root:

```sh
make minivmac
```

The first build clones the pinned Mini vMac source, applies the small
WikiReader performance patch, and generates its 128K configuration under
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
Mac128K.ROM     64 KiB Macintosh 128K ROM
disk1.dsk       first raw 400 KiB floppy image
disk2.dsk       optional second image (up to disk6.dsk)
```

The ROM must be supplied by the user. Disk images are opened writable when the
card permits it and read-only otherwise. Ejecting a disk flushes it.

## Display and controls

The Macintosh's 512x342 monochrome screen is scaled to the upper 240x160
pixels, preserving its aspect ratio. A touch there moves the Mac pointer and
holds its button until release. The bottom strip supplies Command, Shift,
Escape, Return, Space, and Quit. The front buttons are mouse click, Return,
and Escape respectively.

This is an initial bring-up port. It has no sound or full on-screen keyboard,
and performance on physical hardware has not yet been measured. The display
scaler, 68000 dispatch loop, live CPU state, and byte/word read paths use the
C33's zero-wait on-chip RAM. The 512 KiB opcode table is allocated separately
so that the remaining 232-byte CPU state fits there. In a repeatable
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
register `ASR.W` path natively. Register forms of `AND`, `OR`, and `EOR`, plus
quick long data-register arithmetic, now avoid the legacy decoder as well.
Common byte/word reads and long writes inline the core's one-entry guest-memory
translation cache in generated C33 code. Cache misses still call the exact
Mini vMac helpers, which refresh the mapping and preserve MMIO behavior.
Traces contain 16 guest instructions.
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
comparison and branch. This bypasses Mini vMac's generic opcode handler,
lazy-flag dispatch table, and condition callback while preserving the lazy
flags for later instructions. Branches without a proven producer retain the
exact interpreter path; enabling the generic native condition helper remains
slower on this processor.
Translations start in SDRAM; blocks that execute 64 times are re-emitted into
the unused 5.5 KiB IVRAM window. When a displaced trace remains hot, the cache
starts a new promotion generation so transitional boot traces do not remain
there forever. Trace metadata omits an unused mapping pointer and pads each
record to exactly 512 bytes, turning the scheduler's direct-map address
calculation from two multiplies into one shift while still using less memory.
The SDRAM cache retains 1 MiB of generated code in 1024 direct-mapped trace
slots, large enough that routine churn no longer repeatedly discards the
active working set. Promoted immutable-ROM traces can now tail-chain to
another promoted trace while keeping the native stack frame, guest PC, and
cycle count live. Slot identity and cycle admission are checked at every link;
SDRAM traces deliberately retain dispatcher returns because chaining them
increased row-change traffic. The byte-memory mapping fast path also runs from
A0 RAM. Zero-offset generated loads and stores also omit their redundant C33
extension prefix. A deterministic 600-million-instruction boot test reaches
1016 video ticks without alignment, watchdog, guest-state, DMA, or
extension-prefix faults. Its longer steady interval averages about 46.52 ticks
per modeled second, 89.3% above the v7 trace engine's 24.57 result. SDRAM wait
cycles fall from about 934 million to 438 million, and modeled C33 cost falls
from 3.20 to 2.45 cycles per instruction. This is about 77.3% of the original
Macintosh's 60.15 Hz pace, leaving a 1.29x gap.
With the same A0 CPU state and memory routines but `JIT=0`, the
300-million-instruction control run reaches 9.52 ticks per modeled second. The
v8 region build reaches 34.90 ticks per modeled second over the corresponding
startup interval, about 267% above the interpreter.

Build the old interpreter separately for comparison so stale JIT objects are
not reused:

```sh
make -C minivmac clean
make -C minivmac JIT=0
```

For the full-system emulator, create a disposable card without mounting it:

```sh
python3 samo-lib/mbr/make-flash.py build/minivmac/flash.rom
python3 minivmac/make-card.py build/minivmac/card.img \
  /path/to/Macintosh-128K.ROM /path/to/System.dsk
emulator/wremu -g -e build/minivmac/flash.rom \
  -c build/minivmac/card.img
```
