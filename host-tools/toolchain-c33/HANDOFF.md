# C33 toolchain handoff

This is the current operational summary for the C33 toolchain, firmware
build, emulator validation, and remaining work. Detailed ABI rules live in
[`gcc/ABI.md`](gcc/ABI.md); test invocation lives in
[`tests/dejagnu/README.md`](tests/dejagnu/README.md).

## Goal

Replace EPSON's binutils 2.10.1 / GCC 3.3.2 toolchain with binutils 2.47 and
GCC 16.2 while retaining the S1C33E07 PE ABI and the ability to build and run
the complete WikiReader firmware.

## Current status

| Area | Status |
| --- | --- |
| binutils 2.47 | Assembler, linker, BFD, objdump, readelf, CTF, plugins, relocations, and all three C33 core modes work. Exact-source gas/binutils/ld suites have no unexpected failures. |
| GCC 16.2 | Builds the kernel, boot applications, `init.app`, and `wiki.app`; short/long calls, delay slots, `%r15` data addressing, strict alignment, soft-float, variadic forwarding, sibling calls, trampolines, and three core multilibs are implemented. Bit-memory operands are restricted to the base-plus-constant forms the ISA can encode. |
| ABI | New and original objects cross-call in all four compiler combinations and agree at five optimization levels. |
| Firmware | A current full FLASH boot reaches the UI, search results, articles, and scrolling. Modern and shipped firmware render matching screens for the tested workloads. Grifo uses FatFs R0.16 with FAT32/exFAT, multiple volumes, compact fast-seek maps, and 64-bit file positions. |
| Hardware | Run on a real WikiReader with its stock 2009 flash on 2026-09-05 from an 8 GB card: the factory loader loads the gcc 16 kernel; the launcher, ZIM reader (two archives), stock `wiki.app` on a Wikiquote data set, SD DMA, suspend/resume, scrolling, and history across a power cycle all work. Two hardware-only defects were found and fixed (see below). |
| Emulator | The manual-derived ISA, exceptions, interrupts, clocks, SDRAM, SPI, SD card, DMA, LCD, ADC, watchdog, timer, port, and chip-ID models pass `make check`. Seven controller and card overheads are fitted to a real device (2026-09-05); micro-benchmarks agree within 10%. Known divergence: it wakes a HALTed core on the HSDMA terminal-count cause; the silicon does not. |
| DejaGnu | The standard GCC board is authoritative. Focused execution suites are clean; the final post-fix unfiltered run is still pending. |

There is no known wrong-code failure in a supported C or ABI feature. The
remaining unexpected focused results are target-dependent scan or diagnostic
expectations, not malformed code; they remain visible in
[`tests/DEJAGNU-TODO.md`](tests/DEJAGNU-TODO.md).

## Build

Keep binutils and GCC in the same prefix:

```sh
# From the repository root.
host-tools/toolchain-c33/binutils/build.sh host-tools/toolchain-c33/work
host-tools/toolchain-c33/gcc/rebuild.sh
```

The installed tools are under:

```text
host-tools/toolchain-c33/work/install/bin/c33-epson-elf-*
```

`gcc/rebuild.sh` is the normal GCC build. It rebuilds and installs `libgcc`
instead of trusting incremental target-library stamps.

Firmware Makefiles still default to the original compiler. Always select the
modern prefix explicitly:

```sh
make TOOLCHAIN_BIN="$(pwd)/host-tools/toolchain-c33/work/install/bin" <target>
```

Do not mix objects or archives from the two compilers in an incremental
firmware build. Clean the affected component when changing compiler,
optimization level, ABI code, or multilib. The firmware Makefiles do not
track flag changes either: after changing a `-D` define, `touch` the sources
that use it.

A complete rebuild from source, as done on 2026-09-05 for the first card that
went onto hardware: move `host-tools/toolchain-c33/work` aside, keep only the
two tarballs, run the two scripts above (about 19 CPU-minutes), then
`make -C <dir> clean` for `samo-lib/{mini-libc,fatfs,drivers,grifo}`, `wiki`,
and `zim`, and rebuild in that order with `TOOLCHAIN_BIN` set. The kernel came
out byte-identical to the incremental build. `samo-lib/grifo clean` removes
the generated `grifo/include/grifo.h`, which the host `zim-reader` tests
include, so run those after the kernel is rebuilt. `samo-lib/mbr` (the
emulator's `flash.rom`) needs gawk; the device uses its factory flash.

The shipped compiler remains available as an ABI and assembler oracle:

```sh
make toolchain
# installs under host-tools/toolchain-install/bin
```

## Validation

### binutils

The modern assembler matches the original for all comparable code and data:

- 86 hand-written assembly files match in `.text`, `.data`, relocations, and
  ELF C33/core identification;
- 143 original-GCC assembly outputs match in `.text`;
- 20 local relocations differ only in modernized symbol representation and
  produce identical linked code; and
- the current exact-source suites report 338 gas passes, 240 binutils passes,
  and 479 ld passes with zero unexpected failures.

Run the oracle comparison with:

```sh
host-tools/toolchain-c33/tools/compare-with-oracle.sh
```

### GCC and ABI

- `gcc.c-torture/execute`: 24,260 passes, 251 legitimate unsupported results,
  zero failures or unresolved cases across all 1,692 sources and standard
  option variants.
- `gcc.dg/torture` and IPA: no demonstrated backend failure remains.
- LTO: 1,651 passes, 34 external-prerequisite unsupported results, zero
  failures or unresolved cases.
- ABI cross-linking: all four old/new caller/callee combinations agree.
- Differential execution: 200 generated programs per compiler across
  `-O0`, `-O1`, `-O2`, `-O3`, and `-Os` match native reference results.
- The focused C33 target directory reports 408 expected passes and four
  unsupported results, including the indexed bit-memory reload regression.

Useful focused checks:

```sh
host-tools/toolchain-c33/tests/abi/run-abi.sh
cd host-tools/toolchain-c33/tests/dejagnu
./run-dejagnu.sh execute execute.exp=pr61725.c
./run-dejagnu.sh gcc.dg dg.exp=20010516-1.c
```

### Firmware and DMA benchmark

`SD_DMA=YES` is the kernel default. `SD_DMA=NO` retains the same MMC code but
uses its PIO payload loop, providing a matched benchmark build.

The first stable screen is governed by a deliberate two-second deadline:

| Firmware | Modeled time | Executed work |
| --- | ---: | ---: |
| shipped GCC 3.3.2, PIO | 2363.4 ms | 61,364,514 |
| GCC 16.2, PIO | 2370.6 ms | 56,406,351 |
| GCC 16.2, DMA | 2374.3 ms | 40,340,531 |

DMA reduces work by 28.5% there, but the fixed deadline converts the saving
to idle time. Before that wait, the first wiki `File_initialise` occurs at
905.9 ms, 893.6 ms, and 773.0 ms respectively.

The better sustained-I/O measurement opens the first result for `LOVE`. Both
modern paths read the same 395 blocks:

| Kernel path | Article data and LZMA | Executed work |
| --- | ---: | ---: |
| PIO | 3831.4 ms | 64,395,028 |
| DMA | 3629.3 ms | 55,589,491 |

DMA saves 202.1 modeled ms (5.3%) and 13.7% of executed instructions. Final
framebuffers are byte-identical. Milliseconds are predictions from the 60 MHz
MCLK, SPI, DMA, and SDRAM models; real hardware must calibrate absolute card
latency and cross-bank SDRAM overlap.

The DMA backend originally waited in HALT for the HSDMA3 terminal-count
cause. In the emulator that cause wakes the core; on the real chip it never
did, and the first DMA kernel hung on the boot splash while a PIO kernel
booted. The backend now polls the flag with a 20 ms bound; on a timeout it
stops the engines, finishes the block byte by byte with exact accounting of
what arrived, disables DMA for the session, and leaves `dma.txt` on the boot
volume describing the fallback (a healthy boot writes nothing). On hardware
every block of the mount and of the following session completed by DMA. The
2009 Epson Shanghai register sequence in `samo-lib/drivers/src/sd_spi.c`,
which differs only in never writing the IDMA enable register, also worked.

The second hardware-only defect was in grifo's suspend code: gcc 16 spilled
the three saved CMU registers to the stack, which lives in the SDRAM the code
had just switched off, so resume restored garbage clocks. The values now live
in A0 RAM scratch, and the grifo link fails if `.suspend_text` references
`%sp`. Rule for both: the emulator does not know which interrupt causes wake
HALT, and it forgives accesses to a switched-off SDRAM; do not sleep on a DMA
completion cause, and keep suspend-path state out of SDRAM.

The pre-kernel MBR/menu/file-loader still reads by PIO. Kernel block reads use
DMA; card writes remain PIO. The file-loader fits A0 with 371 bytes of live
headroom. The menu is tighter: its BSS ends 18 bytes below the end of A0.

The ZIM reader uses Grifo's FatFs service rather than parsing a filesystem
itself. Its card layout keeps the boot chain on a FAT32 first partition, with
`zim.app` and its Kiwix icon beside the stock `wiki.app` in `init.ini`, and
stores any number of `*.zim` archives on a second exFAT partition, chosen at
run time through the original wiki-selection screen. A contiguous exFAT file
produces its 16-byte seek map directly from filesystem metadata; fragmented
files still use FatFs's normal chain traversal. The single-volume FAT32
layout remains a fallback for archives below 4 GiB. `zim/make-card-image
--wiki DIR` also installs a native data set for the stock app. The complete
124 GB English Wikipedia archive (27.2 M entries) runs in the emulator; that
needed 27-bit article ids, per-capitalization prefix probes, and a placeholder
budget so picture-heavy articles fit the 512 KiB stream. It has not yet been
written to a card.

The current contiguous 944 MiB archive has been tested through full FLASH
boot, prefix search, and article rendering. With current binaries, direct
Grifo boot reaches the ZIM parser at 667.9 modeled ms from exFAT versus 1793.5
ms from FAT32; the latter reads 1,889 FAT sectors to construct its seek map.
The 64-bit path also opens a 4.5 GiB exFAT test archive whose live path index
was relocated to byte 4,300,000,000, proving an actual seek and read above the
4 GiB boundary. Image-rich ZIMs are supported through a portable WebP decoder:
assets are scaled to the display, alpha-composited on white, Atkinson-dithered,
and embedded in the original one-bit article stream. Images are decoded on
demand with one screen of render lookahead; direct luma decoding makes the
first Paris photograph 33% faster, and the initial article screen appears in
16.60 instead of 40.35 modeled seconds. A full-FLASH Wikivoyage test completes
with no watchdog timeout.
`zim/make-card-image` creates the dual-volume image on macOS.

### GCC 16 optimization benchmark

A clean full-FLASH `LOVE` search/article A/B rebuilt the complete runtime stack
(`kernel.elf`, `init.app`, and `wiki.app`) at either `-O2` or `-Os`. The A0
MBR/menu/file-loader remained at their required `-Os` in both images. Each run
repeated exactly, and the final framebuffers have the same SHA-256 digest.

| Metric | `-O2` | `-Os` | `-Os` change |
| --- | ---: | ---: | ---: |
| installed runtime files | 215,172 B | 195,664 B | -9.1% |
| reset to wiki main loop | 786.7 ms | 798.3 ms | +1.5% |
| article interval, instructions | 55,588,282 | 52,724,316 | -5.2% |
| article interval, modeled time | 3680.09 ms | 3796.75 ms | +3.2% |
| reset to article completion, instructions | 136,677,103 | 132,298,914 | -3.2% |
| reset to article completion, modeled time | 11860.6 ms | 12057.8 ms | +1.7% |
| modeled SDRAM wait cycles | 343,337,791 | 361,692,295 | +5.3% |

Keep `-O2` as the runtime default. On the current model, `-Os` executes less
code but loses more time to SDRAM/bus stalls. This is a medium-confidence
ordering, not a hardware measurement: absolute SD latency and conservative
cross-bank overlap still need physical calibration.

### ZIM article load benchmark

The memory system, not the instruction count, bounds the ZIM reader: the
core has no cache, and the model's SDRAM row changes and refreshes made the
`Cat` load run at 4.65 cycles per instruction. On 2026-09-05 the emulator
gained cycle- and row-activation-weighted profiles (`-F`, `--- window
sdram`), and the reader, the Zstandard port, the kernel's SD DMA backend, and
the SDRAM controller timing were changed accordingly; details and the phase
breakdown are in [`../../zim/README.md`](../../zim/README.md).

| Measurement (calibrated model, 2026-09-05 evening) | Before | After |
| --- | ---: | ---: |
| `Cat`, tap to first painted page | 2864.0 ms | 1303.5 ms |
| same on the device (`ZIM_BENCH`, tap to paint), 2026-09-05 then 09-06 | 1846 ms | 1390 ms |
| `Tokyo` (Japanese first line), tap to paint, emulator / device | 8052 ms | 1141 / 1030 ms |
| Wikivoyage `Paris`, first photograph decode | 1861.0 ms | 1565.5 ms |
| app start to keyboard painted | 807 ms | 632 ms |

(The manual-only model used earlier that day gave 1941.3 and 1253.9 ms for
the `Cat` rows and 1342.5 and 1084.0 ms for `Paris`.) The emulator's ten
fitted timing parameters, the fitting tool, and the device/model ratios are
in `emulator/README.md`, "Calibration"; the device's benchmark file is
`zim/bench-device-2026-09-05.txt`.

Screens and article streams are byte-identical, eight decoded articles hash
identically to `zimdump`, and `host-tools/zim-reader/make check` passes. The
kernel's SDRAM retiming (`samo-lib/grifo/src/sdram.c`, `SDRAM_TIMING=FAST`
default, `STOCK` to disable) and the DMA descriptor move to DSTRAM ran on the
device the same day (boots, searches, opens articles, subjectively faster;
not yet timed); the loader in flash still programs the stock values, so the
kernel reprograms the controller from A0 RAM after boot, and SuspendCode
restores the same refresh interval on every wake from the event loop's idle
sleep. The reader's `ZIM_BENCH=YES` build writes per-phase load times and
memory/card micro-benchmarks to `bench.txt` on the card, on the device and in
the emulator alike, for calibrating the model (`zim/bench-compare`).

## Known boundaries

### Toolchain feature debt

- `__int128` needs a C33 ABI, TImode lowering, and libgcc helpers.
- Atomics need a target concurrency model and libatomic; C33 has no native
  compare-and-swap and this toolchain is single-threaded.
- GCC heap trampolines need allocator and executable-memory runtime hooks.
- Full debugger use has not been qualified. DWARF line tables and ordinary
  `.debug_info` are valid, but production firmware is stripped.
- Darwin intentional-crash, SARIF, and diagnostic-path cases need a focused
  host-integration audit.

### External runtime prerequisites

These are not GCC/binutils correctness bugs and require separate approval:

- C99 libm and floating-point `printf`;
- hosted file, environment, time, signal, and process APIs;
- persistent gcov output and `__gcov_exit` transport;
- sanitizer runtimes;
- crt iteration of constructor/destructor arrays; and
- semihosting, persistent host files, or a thread/TLS runtime.

### Emulator boundaries

- The SDRAM model serializes cross-bank command/data phases conservatively.
- Absolute SD-card latency and the SDRAM controller's per-access overheads
  are fitted, not derived: `emulator/src/model.c`, one board, one card.
- HALT wake-up: the model wakes the core on any enabled ITC cause, including
  the HSDMA terminal count, which the hardware did not do. Firmware should
  poll DMA completion.
- SDRAM with the controller's application unit off: accesses return the
  underlying cells in the model; the hardware returns nothing useful.
- Firmware plus differential tests execute 57 of 74 implemented PE
  operations; focused core tests cover most of the remainder. Targeted
  independent coverage of stack-special and indirect-jump forms is still
  useful.
- Illegal delay-slot behavior has no unstable-state model.
- Coprocessor instructions are intentionally absent because the S1C33E07 has
  no attached coprocessor.
- Unused alternate pins, timer output modes, DMA triggers, and the RTC are not
  implementation priorities unless WikiReader firmware begins using them.

## Next work

1. The A0 RAM decoder ran on the device on 2026-09-06 (`Cat` 1390 ms,
   `Tokyo` 1030 ms, no DMA fallback), and A0 RAM tests were added to the
   benchmark and fitted: internal-RAM fetch is free, a taken branch there
   is four cycles. The model now predicts `Cat` within 3%; its weakest
   spot is a read after a write (about 15% under on those patterns).
2. Exercise suspend/resume and a long session on the retimed kernel. The
   emulator is calibrated to one early 32 MB board; a `bench.txt` from a
   16 MB V4 board run through `emulator/tools/fit_model.py` would show
   whether the fitted overheads differ between revisions. The first article
   after a cold boot pays about 1.8 s filling the glyph caches from the
   card; that was the missing fast-seek map on the font files, fixed the
   same evening, together with sector-sized glyph fills.
3. `html_to_text` is now the largest remaining cost after the decoder
   (about 260 ms of `Cat`, half of it instruction fetch): too large for the
   remaining 700 bytes of A0 RAM, so the next step is a smaller hot loop.
2. Write the 124 GB English Wikipedia image to a 128 GB card and repeat the
   hardware checklist; time a Cat load on hardware with `SD_DMA=YES` and
   `SD_DMA=NO` to calibrate the SD model.
2. Make modern-vs-legacy firmware toolchain selection explicit and resistant
   to stale mixed objects, and make the firmware Makefiles notice flag
   changes.
3. Further reader speed, in order of expected value: overlap the next
   4 KiB input slice's card read with decoding (the kernel's file read is
   synchronous, so this needs an asynchronous block read); decode the four
   Huffman literal streams one after another instead of interleaved (the
   interleave exists for superscalar cores and costs a row change per byte
   here); shrink the sequence loop's 83 instructions per sequence; and, if
   the hardware confirms the timing model, place the hot sequence loop in
   the 5.5 KB of A0 RAM the suspend code does not use.
4. Add targeted independent runtime coverage for the implemented PE
   operations not reached by firmware or differential programs.
5. Exercise an unstripped C33 program with a real debugger and verify
   stepping, frames, arguments, and variables.
6. Refine documented cross-bank SDRAM overlap once hardware timings exist.
7. Run the complete post-fix DejaGnu suite when the multi-hour validation is
   wanted, then treat its fresh failures as the only broad-suite backlog.
8. Add link-time A0 size assertions, especially for the menu.
8. Exercise the ZIM reader with a complete full-English archive and replace
   its fixed 512 KiB decoded-article buffer if real articles exceed it.
9. Consider a small decoded-image cache if navigation repeatedly revisits the
   same images; the current lazy path intentionally trades later scroll pauses
   for much faster initial presentation.

## Source layout

```text
binutils/files/             C33 binutils target sources
binutils/build.sh           binutils 2.47 build/install
gcc/files/                  C33 GCC backend sources
gcc/patches/                focused upstream GCC correctness patches
gcc/rebuild.sh              complete GCC 16.2 and libgcc rebuild/install
gcc/ABI.md                  ABI and ISA contract
tests/abi/                  old/new cross-link probes
tests/dejagnu/              standard GCC board and runner
tests/DEJAGNU-TODO.md       current compiler/test capability backlog
tools/compare-with-oracle.sh assembler comparison against 2.10.1
```
