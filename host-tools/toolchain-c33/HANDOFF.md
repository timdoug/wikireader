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
| Hardware | Run on two real WikiReaders, both early 32 MB boards, most recently 2026-09-07: the factory loader loads the gcc 16 kernel; the launcher, ZIM reader (two archives), stock `wiki.app` on a Wikiquote data set, SD DMA, suspend/resume, scrolling, and history across a power cycle all work. `Cat` opens in 942 ms, from 1846 ms two days earlier. Both units measure a 4 MB SDRAM bank stride that the controller's ADDRC field does not admit, and the reader now measures it rather than deriving it. Three hardware-only defects were found and fixed (see below). |
| Emulator | The manual-derived ISA, exceptions, interrupts, clocks, SDRAM, SPI, SD card, DMA, LCD, ADC, watchdog, timer, port, and chip-ID models pass `make check`. Twelve controller and card overheads are fitted to a real device (2026-09-07, `WREMU_BOARD_REV=7` for the 32 MB board under test); every micro-benchmark agrees within 12% and most within 5%, while the reader's article phases are still 20 to 33% faster than the device for reasons no measurement explains. Known divergence: it wakes a HALTed core on the HSDMA terminal-count cause; the silicon does not. |
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

| | 2026-09-05 morning | 09-06 midday | 09-06 night |
| --- | ---: | ---: | ---: |
| `Cat` (Simple English) | 1846 ms | 1245 ms | 1121 ms |
| `Tokyo` (Japanese first line) | about 8 s | 920 ms | 747 ms |
| `Japanese Bobtail` | | | 1280 ms |
| cached reopen | 63 ms | 65 ms | |

The night column is the fixed build (`926d221c`) on the device, verified
by the phase byte counts: `Japanese Bobtail` reports 29549 raw, 920 text
and 765 stream bytes, which is what the corrected decode produces in the
emulator (the broken build produced 3852 bytes of text from the same
article because the garbled markup leaked tag fragments into the page).

The device is 25 to 40% slower than the model in every phase of that run
(Zstandard 500 ms against 393, HTML 165 against 121, wrap 93 against 80),
wider than the 15% residue recorded earlier in the day. The likely cause
is bank geometry: `zim_blob.c` and `search.c` choose banks by absolute
index, tuned on the emulator's 16 MB board with 4 MB banks, while the
device is an early 32 MB board with 8 MB banks, where reaching bank 2
also forces a filler allocation of about 15 MB. Placement should be
relative ("a different bank from this one") rather than absolute.

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

## Session 2026-09-07: a decoder bug, the boards measured, and a refit

Three things happened, in this order.

**A decoder bug of mine, found on the device.** `Japanese Bobtail`
rendered as scrambled words. The article decoded to `bb1f39d8` where the
archive says `83d60b5f`, identically on the device and in the emulator,
so it was arithmetic and not memory. Packing an FSE table entry into one
word leaves 14 bits for the base value; larger bases are marked and
recomputed from the extra-bit count, and the rule differs per table:
literal length `1 << bits`, match length `(1 << bits) + 3`, offset
`(1 << bits) - 3`. The match-length rule was missing, so every match of
16387 bytes or more decoded as 16383. Only articles containing such a
match were damaged, and none of the eight in the hash suite did. Fixed in
`926d221c`; the trace build now unpacks every entry and compares it with
the table it came from, and the suite includes that article.

The hunt took hours longer than it should have because of an install
fault, not the bug: macOS mounts a second copy of an already-attached
image as `VOLUME 1`, so a plain attach-and-copy wrote to a stale mount
while the emulator read the image file. Every experiment ran the previous
app, which produced a bisect that "proved" the corruption survived
reverting all of my work. `/tmp/install.sh` and `/tmp/buildapp.sh` now
detach stale mounts, copy, sync and compare the bytes back.

A second, smaller bug came out of it: revisiting an article moved its
history entry to the top but never refreshed the stored title, so an
article first opened while the decode was broken kept a garbled title for
ever (`5ca435be`).

**The boards, measured rather than assumed.** The benchmark build gained
a hardware probe: the controller's claimed geometry, whether the address
window repeats (the true memory size), where the heap hands out blocks,
and a read-pair latency sweep from 4 bytes to 16 MB. Both of the user's
WikiReaders report the same thing, and the sweep contradicts the
register. Rows are 1 KB as the manual says, but banks repeat every 4 MB
where ADDRC 3's geometry implies 8 MB: reads 4, 8 and 16 MB apart all run
at the speed of reads within one row. The board carries a single memory
device, so the reason is not established, and the code follows the
measurement. `zim/probe-compare` summarises one or more `bench.txt`
files, one section per run, so a card carried between units reports both.

Two consequences. The reader now measures the bank stride at runtime
instead of deriving it, and places buffers relative to each other
(`zim_alloc_other_bank(size, avoid)`, walking forward one bank at a time)
rather than by absolute bank number, which on a 32 MB board had been
asking for a filler of about 15 MB. And the emulator can be the board
under test: `WREMU_BOARD_REV=7` boots it as a 32 MB V3 unit, port A
having been unmodelled until now, with the 32 MB geometry set from the
measurement (eight banks of 4 MB).

**A refit, and what it did not fix.** Micro-benchmarks were added for
what the earlier fit never exercised: data accesses issued by code
running from internal RAM, and reads the data queue already holds. The
device does the first in 2.6 cycles where the model had 1.6 and pays for
the second where the model served it free. Two parameters cover them,
`dq_iram_extra` and `dq_hit`; the refit also moved `wr_rd_turn` from 6 to
3, `wr_ticks` to 0, `dq_extra` to 1, `dma_extra` to 30 and
`sd_read_latency` to 60000. Micro-benchmark error fell from 0.286 to
0.040, every test within 12% of the device and most within 5%.

The article phases stayed 20 to 33% faster in the model than on the
device (decode 411 ms against 496, converter 125 against 166, wrapper 82
against 95). A load-use pipeline interlock was the obvious explanation
and the device refutes it: dependent and independent loads both cost 2.1
cycles, exactly as modelled. Nothing measurable accounts for the gap, so
the model is for ranking changes, not for predicting device
milliseconds, and anything that matters gets a device run.

Device results, tap to painted page, `ZIM_BENCH` build, all on the same
32 MB unit:

| | 09-06 morning | 09-06 night | 09-07 |
| --- | ---: | ---: | ---: |
| `Cat` | 1245 ms | 1121 ms | 942 ms |
| `Tokyo` | 920 ms | 747 ms | 730 ms |
| `Japanese Bobtail` | | 1280 ms | 798 ms |

Recipes that changed:

- Emulator runs that should match the hardware need `WREMU_BOARD_REV=7`;
  without it the emulator is a 16 MB rev 8 board with 4 MB banks.
- `tools/fit_model.py DEVICE-bench.txt CARD.dmg` now runs the emulator as
  that board and fits twelve parameters; `branch_taken_iram` is pinned
  because three tests measure it exactly.
- Install into a card image or onto the card through the verified step,
  never a bare `cp`.

## Session 2026-09-07 afternoon: the decoder, and what the clusters cost

Modeled `Cat` (the emulator as the 32 MB board) went 793 to 715 ms over
eight commits (6b0bc7e9..13041524), every step checked against the 27
article, text and stream hashes and the host `make check`.  None of it
has run on the device yet.

**The decode phase is mostly neighbours.**  A Python ZIM parser
(`/tmp/zimpos.py`) put numbers to it: Kiwix packs about 85 articles into
each 2 MB cluster and Zstandard decodes only from the cluster start, so
the reader produces 872 KB to reach the 130 KB of `Cat`, 1.7 MB for the
30 KB of `Japanese Bobtail`.  The 496 ms "sequence loop" on the device
is that.  Re-clustering at card-build time was measured on three
clusters: 256 KB clusters cost +34% of text size (still a valid ZIM),
128 KB +53%; a 256 KB shared dictionary with one article per cluster
costs -1%, but is no longer a ZIM.  The user chose to keep stock
archives and speed up the decoder; that decision stands.

Also corrected: the `Cat` load is 29,801 sequences of about 29 bytes,
not 200k (seven instructions shared the entry-load line in the line
profile), and 84% of them carry a fresh offset, so the repeat-offset
path is not the common case.

**What was done, in order.**

- The sequence loop as C with every piece of state in a local: GCC still
  spilled the states and pointers on every sequence (common path 239 to
  211 instructions).  So `zim/zstd/zstd_c33_seq.s`: a hand-written loop
  in A0 RAM with the bit container, bits consumed, output and literal
  pointers, the last offset and the count in registers, the states and
  bounds in a frame on the DSTRAM stack, and the sequences near the end
  of a block handed back to C for `ZSTD_execSequenceEnd`.  The compact
  table words were repacked for it (next state in the top bits, the two
  shift amounts stored instead of the bit counts, no offset base at all:
  it is `(1 << bits) - 3`).  The build rules accept `.s` sources now
  (`application-post.mk`), and the emulator prints the first misaligned
  accesses with their PC, which found the one bug (the destination-
  aligning batch clobbered the register holding the misalignment).
- The FSE table builder writes only the packed words: the symbol order
  is spread into the compact table's IVRAM slot and overwritten in
  place, and the per-symbol tables live in `zim_fast_scratch`, 640 bytes
  of `.fastbss` in A0 RAM shared with the wrapper's width and word-break
  tables (the two never run at once).  1.72M to 1.13M cycles.
- Placement: the text buffer in a bank other than the article buffer's
  (the wrapper copies every word from one to the other), the link table
  likewise, the literal scratch buffer away from both output and input.
  The literal move made no measurable difference: most activations in
  the loop are refresh re-opens (every 288 clocks every bank closes),
  not ping-pong, so spreading data over more banks is a wash.
- `bmf_char_width` reads a width from a resident or cached glyph record
  without `pres_bmfbm` copying the record out.
- One Huffman decoder (`HUF_FORCE_DECOMPRESS_X1`; the double-symbol one
  ran from SDRAM with a 16 KB table), its four streams decoded in turn
  (interleaved, every output byte and most reloads opened a row), its
  table in a reader-placed buffer in the bank idle during literal decode
  (`ZSTD_DCtx_setHufTableBuffer`).  26k fewer activations, only 3.5 ms:
  the loop is still 28 instructions a symbol.
- The fresh-offset path falls through in the loop (three taken branches
  fewer per sequence).
- The converter parses attributes on the way to the '>' in one pass
  (entered only for opening tags inside `<main>`), which also fixes a
  '>' inside a quoted value ending the tag and a bare attribute
  swallowing the next; over 51 saved articles only one output changed,
  losing a garbage line.  729 to 715 ms.

**Where 715 ms goes** (model cycles): the sequence loop 14.9M, of which
decoding is about 175 cycles a sequence, bounds checks 35 and copies
290 (a load from SDRAM by code in internal RAM costs 4 to 5 cycles, and
the 8-register batches are near their floor); the three overlays (HTML
9730k for html, wrap and Huffman together); the table builder 1.1M;
`memcpy` 0.9M (glyph painting and card reads); `HUF_readDTableX1_wksp`
0.84M with a third of it fetch wait; the card's SPI path 1.5M plus
0.67M of `delay_us` powering the card back up after idle; `bank_size`
0.4M once per boot.

Traps met: a failed build left the previous app in the hash image and
the suite "passed" (check both lines); a chained shell command that
`cd`s leaves later commands elsewhere; turning a local array into a
pointer leaves `sizeof` at 4 (twice: the log2 table copy in the
builder, the second ASCII-width copy at the wrapper's font switch,
which changed every stream hash and was chased through DSTRAM and A0
placements before the real cause); `objdump -l` on the linked ELF mixes
the three overlays' line numbers (same VMA), so take them from the
object file with base 0x81a00 (`/tmp/ovlprof.py`).

### Device run, 2026-09-07 evening

The afternoon's changes on the user's 32 MB unit, same articles in the
same order, tap to painted page:

| | morning build | evening build |
| --- | ---: | ---: |
| `Cat` | 993.7 ms | 871.6 ms |
| `Tokyo` | 772.2 ms | 705.0 ms |
| `Japanese Bobtail` | | 1087.6 ms |
| `Cat` revisited | | 60.7 ms |

By phase, the decoder is 20% faster on both articles (`Cat` 495.6 to
396.8 ms, `Tokyo` 223.4 to 179.2) and the converter 11% (166.5 to 148.7,
135.9 to 121.0); the wrapper gained 6% on `Cat` and painting is
unchanged.  `Japanese Bobtail` has no comparable earlier figure: it
decodes 1.7 MB of cluster to reach a 30 KB article, and its decode rate
matches `Cat`'s exactly, so the 798 ms recorded earlier in the day was
almost certainly a cluster that was already open.

The first run of the evening build was 124 ms *slower* on `Tokyo` than
the morning's, with every measured phase faster and 201 ms in the blob
phase that no timer accounted for, against 14 ms before.  The cause was
the literal scratch buffer and the literal Huffman table being placed
through the allocator's bank walk on every cluster open: the walk holds
space with multi-megabyte fillers, and what it costs depends on the state
of the heap, which is why the emulator never showed it.  Neither buffer
depends on the cluster, so both are now placed once and kept, and `Tokyo`
came back at 705 ms with 10.8 ms unaccounted.  Two lessons: an allocation
whose cost depends on heap state does not belong on a per-article path,
and a benchmark that reports phases as sums of slots should print what
the slots do not cover.  The reads of the archive's index, including the
directory entry that starts every load, were also outside the card timer
and are inside it now.

**The wrapper's cost on `Tokyo` is font I/O.** 233.9 ms of its 705, seven
times `Cat`'s cost per byte of text.  A window profile of that phase in
the emulator is 60% card transfer (`spi_transmit` 33%, `rcvr_datablock`
15%, `spi_receive` 6%) with `load_bmf` and `pres_bmfbm` behind it: the
article's Japanese runs open a CJK font and pull glyph metric records
from the card while word widths are measured.  It is already far better
than it was (8.0 s before the per-font seek map and sector windows), but
it is now the largest single item after the decoder for any article with
non-Latin text, and it is the obvious next target.

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

The subsequent startup/image round and returned hardware results are in
[`zim/PERFORMANCE.md`](../../zim/PERFORMANCE.md). The compact C33 `memset`
loop cuts emulator initialization by 5.4%; WebP/dither overlays and decoder
changes cut its first Paris image time by 29.4%. A subsequent hardware A/B
pair with identical timers measures initialization 828.7 to 749.7 ms (9.5%
less time), first Paris image setup/decode/dither 1476.3 to 1107.6 ms (25.0%
less), and full Paris page including benchmark I/O 4183.4 to 3844.5 ms (8.1%
less). Cat is unchanged at 869.5 versus 871.5 ms. Both article hashes and
the image bitmap hash match. There is one complete A/B pair; raw logs are
`zim/bench-device-2026-09-07-ab-{before,after}.txt`. The earlier text controls
also measured Tokyo 704.9 ms, Japanese Bobtail 1089.0 ms, cached Cat 60.3 ms.
The physical boot volume uses 4096-byte FAT clusters versus the emulator
image's 512 bytes, which matters for startup/font-map comparisons.

Ranked against the device's 871.6 ms for `Cat` and 705.0 for `Tokyo`:

1. Glyph metrics for non-Latin text: 234 ms of `Tokyo`, and nothing on
   `Cat`.  Measuring a word's width pulls whole 56-byte glyph records
   from the card through `pres_bmfbm`; widths need one byte of each.  A
   width-only cache, or reading the metric header separately from the
   bitmap, would cut most of the transfer.  Check first whether the cost
   is the initial `load_bmf` of the CJK font or the per-glyph windows.
2. The Huffman literal loop in assembly: 28 instructions a symbol, about
   19 reachable with the bit reader in registers.  Roughly 12 ms of
   `Cat`'s 397 ms of decoding.
3. The SDRAM refresh interval: the kernel programs 0x120 clocks where the
   part allows 468 at 60 MHz, and about 140k refreshes fall in a `Cat`
   load, each closing every bank.  10 to 17 ms, and it needs a soak.
4. The sequence loop's decode side: state loads through the frame, the
   three bounds checks, the refill's four byte loads.  Perhaps 10 ms.
5. Overlap card reads with decoding: 133 ms of card time per `Cat` load,
   about 40% of it hideable behind the decode.
6. Unchanged: the model's optimism against the device, suspend/resume
   soak, the 124 GB card, the toolchain items.

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
