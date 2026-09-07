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

## Session handoff, 2026-09-05 to 06: reader speed and calibration

What was done, in order, each step committed and verified (eight decoded
articles hash-identical to `zimdump`, screens pixel-identical, host checks,
emulator `make check`):

1. Cycle- and SDRAM-row-weighted profiling in `wremu` (`-F` columns,
   `--- window sdram` lines) showed the reader was bound by SDRAM row
   changes, refresh, and instruction fetch, not instruction count. Fixes:
   bank-local cluster buffer, literals kept out of the output bank, batched
   copies, lazy bit-stream refills in the Zstandard port; the kernel
   retimes the SDRAM controller to the part's data-sheet values from A0
   RAM (`SDRAM_TIMING=FAST`, `src/sdram.c`; SuspendCode restores the
   refresh value on wake) and keeps SD DMA descriptors in DSTRAM.
2. The reader's `ZIM_BENCH=YES` build times itself on the device (memory
   and card micro-benchmarks at start, per-phase article loads) and writes
   `bench.txt` to the card; install it as `zimbench.app` with the inverted
   kiwi icon. The device files are `zim/bench-device-2026-09-05.txt` and
   `-06.txt`. It must run on a writable image in the emulator.
3. `emulator/tools/fit_model.py` fitted ten timing parameters
   (`emulator/src/model.c`, `WREMU_MODEL=` overrides) to those files; the
   manual-only model had been optimistic by a third. Facts established:
   a taken branch costs 5 cycles from SDRAM and 4 from internal RAM;
   fetch from A0 RAM, IVRAM, and DSTRAM is exactly one cycle; the card
   takes 1.2 ms per read command; a read after a write waits about six
   ticks. Micro-benchmarks agree within 5% except two store-then-load
   patterns; `Cat` within 10%.
4. Code placement in internal RAM: the Zstandard sequence loop and FSE
   table builder in the 5 KB of A0 RAM the kernel leaves at 0xc00
   (`.fastcode`, `application.lds`); the HTML converter, wrapper, and
   Huffman literal decoder as overlays in the LCD window buffer at 0x81a00
   (ld `OVERLAY`, `zim/zim_overlay.c`, a post-link `objcopy` because the
   kernel's loader places sections by address and C33 calls are
   PC-relative even in their long form). Font files got fast-seek maps and
   sector-sized glyph fills (a glyph miss had walked the FAT chain from the
   start of a 3.6 MB font). Cluster slices grew to 16 KiB.

Device results, tap to painted page, `ZIM_BENCH` build:

| | 2026-09-05 morning | end of 09-06 |
| --- | ---: | ---: |
| `Cat` (Simple English) | 1846 ms | 1245 ms |
| `Tokyo` (Japanese first line) | about 8 s | 920 ms |
| cached reopen | 63 ms | 65 ms |

Calibrated-model `Cat` window (`retrieve_article` to
`render_article_with_pcf`): 2864 ms for the morning's firmware, 1073 ms now.

Recipes that work (details in `emulator/README.md` and `zim/README.md`):

- Build: `make TOOLCHAIN_BIN=$PWD/host-tools/toolchain-c33/work/install/bin
  mini-libc fatfs drivers grifo`, then `cd zim && make TOOLCHAIN_BIN=...`;
  add `ZIM_BENCH=YES` for the benchmark app, `OPT="-O2 -DZIM_TRACE_HASH"`
  for the hash-printing app. Remove `zim/build/*.o` when changing flags.
- Card image: `./zim/make-card-image ARCHIVE.zim /tmp/x.dmg` (7 s). The
  device card's boot volume takes `kernel.elf`, `zim.app`, `zimbench.app`
  by plain copy.
- Article benchmark: from `emulator/`, `./wremu -R -e ../samo-lib/mbr/flash.rom
  -c /tmp/x.dmg -T 40,36,100000000 -K 300000000,CAT -T 30,40,500000000
  -Y 0x<retrieve_article>,0x<render_article_with_pcf> -F prof.txt
  -n 1200000000`, addresses from `zim/zim.map`. Symbolise `prof.txt` with an
  unstripped relink of the same objects (the `ld` line from
  `make -n -W build/zim.o zim.app` without `-s --strip-all`), `nm -n`, and
  `addr2line`.
- Benchmark app in the emulator: copy the image first and run it without
  `-R`, with the typing at `-K 600000000` and the tap at 900000000 since
  the benchmarks delay the keyboard; compare with `zim/bench-compare`.
- Hash check: eight parallel emulator runs of the trace build, one word
  each (CAT DOG WATER MUSIC EARTH LONDON PARIS APPLE), against FNV-1a of
  `host-tools/zim-reader/zimdump ARCHIVE blob C Path`.

Pitfalls met, so they need not be met again:

- The shell's working directory drifts between tool calls; use absolute
  paths.
- `make check` at the repository root is not the emulator's; run it in
  `emulator/`.
- A read-only card image breaks the benchmark app's FatFs after its first
  rejected write and the run ends in a font panic.
- Scripted tap release is delivered only when the emulator idles; a faster
  build shows thousands of extra render calls, not a bug.
- The user's device is an early 32 MB board (SDRAMC ADDRC 3); the emulator
  boots as a 16 MB V4.
- HSDMA into A0 RAM did not complete; `sd_dma.c` takes the byte path for
  any destination below SDRAM.
- Moving a function into A0 RAM only helps if it was not being inlined:
  small helpers marked `noinline` there made the converter slower.

### Session 2026-09-06 evening: 1073 to 761 ms in the model

The changes are listed in `zim/README.md` ("A third round"); the
commits are `fa2f88c9` onwards. Method that worked: the emulator's new
row and row-pair histograms (`WREMU_ROWHIST=1`) named the objects that
alternated rows, the probe caller list named who called `memcpy`, and
every decoder change was checked by the 24 hashes (article, text and
stream for eight words) from the `ZIM_TRACE_HASH` build. Pitfalls added:

- `-F` buckets aliased internal-RAM code onto the kernel until 6dc2dada;
  profiles from before it charge the A0 loop with kernel cycles.
- A function placed in A0 RAM or an overlay must be `noinline` when its
  only caller is elsewhere, or the compiler inlines it out of there.
- Inline asm that clobbers every call-clobbered register cannot be inlined
  itself; keep such wrappers `noinline` or the trace build fails.
- The background hash job once ran against a stale app, so a "verified"
  result must come from a build whose `zim.app` timestamp matches.
- Another session commits to this repository concurrently (72941895,
  19f0f289 on 2026-09-06); its kernel adds a syscall, so images with an
  older `kernel.elf` panic with "undefined syscall" at the first key.
- The device has not run anything after commit c59f999b; the DSTRAM
  stack and the compact tables need the benchmark build on the device
  before they are trusted.
- The compact table packing marks base values that exceed its 14-bit
  field and recomputes them from the extra-bit count. The rule differs
  per table (literal length `1 << bits`, match length `+ 3`, offset
  `- 3`); missing the match-length rule corrupted only articles with
  matches of 16 KB or more, which the eight-article hash suite did not
  contain. The trace build now verifies every packed entry against the
  8-byte table (`ZSTD_c33_verifyCompact`), and the suite includes
  `Japanese Bobtail`.
- Install into a card image through a step that detaches stale mounts,
  copies, and compares afterwards. macOS attaches a second copy of an
  already-attached image under a `VOLUME 1` name, so a plain
  `hdiutil attach` plus `cp` can write to a mount that is not the image
  the emulator then reads. Hours went into a bisect whose builds never
  reached the image; every "still broken" result was the previous app.

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
| `Cat`, tap to first painted page | 2864.0 ms | 1073.4 ms (760.6 by the evening of 09-06, device untested) |
| same on the device (`ZIM_BENCH`, tap to paint), 2026-09-05 then 09-06 | 1846 ms | 1245 ms |
| `Tokyo` (Japanese first line), tap to paint, emulator / device | 8052 ms | 980 / 920 ms |
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

Ranked for the reader's speed, all measurable in the calibrated emulator
before touching the device (`Cat` 761 ms in the model at the end of
2026-09-06):

1. Run the benchmark build on the device: the DSTRAM stack, the compact
   IVRAM tables and the bank placement are modeled only. Compare with
   `zim/bench-compare`; the 32 MB board's 8 MB banks put the context and
   input in an otherwise empty bank 1.
2. The sequence loop is 282 ms: about 50 instructions per sequence of bit
   reads and reload checks (`BIT_lookBitsFast`, `ZSTD_reloadIfNeededC33`,
   the state updates), 100 cycles of copies, and the execSequence checks.
   A hand-written inner loop keeping the bit container and the three
   states in registers is the remaining large item; the copies' lead byte
   batch for a misaligned destination (15% of the realigned path) could be
   partial stores instead.
3. The converter (128 ms, 27 instructions per byte in `html_to_text`) and
   the wrapper (74 ms, 37 per text byte in `wrap_body`): the attribute
   bytes are scanned twice, text runs are copied byte by byte, and the
   wrapper scans, measures and copies each word in three passes.
4. The literal phase (62 ms): the Huffman table builders run from SDRAM
   (`HUF_readDTableX1_wksp` 16 ms, the weight decoder 6 ms, with 64-bit
   entry replication the core does with library multiplies); sequential
   streams would shrink the decoder's overlay enough to hold the builder.
5. The kernel's card path (about 35 ms): `spi_transmit` and `delay_us`
   straddle a row boundary, the token hunt runs from SDRAM, and the card
   is re-initialised after every suspend (`CARD_POWER=OFF`, the other
   session's measured choice).
6. Overlap card reads with decoding, now that the DMA cadence is known to
   be about 75 cycles per byte with the CPU stalled for the bus: only the
   32-cycle shift per byte is hideable, so about 40% of the card time.
7. Calibration residue and the 16 MB board `bench.txt`; suspend/resume
   soak on the retimed kernel; the 124 GB card; the toolchain items from
   before (modern-vs-legacy selection, flag-change detection, PE runtime
   coverage, debugger session, full DejaGnu run).

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
