# Fallback-font startup performance, 2026-09-07

Baseline: `eeebbdd4`, which commits the third round's hardware-verified WebP
improvements and results. This round loads only the headers of the three
large fallback fonts at startup and reads their glyphs through the existing
cache. Normal emulator initialization is 148 ms shorter (8.3%), and the
reader allocates and preloads 42 KiB less font data. The returned hardware
A/B pair confirms 79.2 ms less initialization time (11.9%). Cat and Tokyo
change by at most 2.1 ms; full Paris loading is 15.6 ms longer (0.4%) in
this sample.

## Returned hardware results

Raw logs: [A BEFORE](bench-device-2026-09-07-round4-ab-before.txt) and
[B AFTER](bench-device-2026-09-07-round4-ab-after.txt). The card's normal
reader, both benchmark apps and their icons match the staged SHA-256 hashes;
the kernel is unchanged. The logs identify the exact emulator-tested
18:41:47 and 18:42:36 builds, GCC 16.2.0, the same 32 MB board settings and
60 ticks/us timer, and Simple English at initialization.

| Hardware measurement | A BEFORE | B AFTER | Difference |
| --- | ---: | ---: | ---: |
| Reader initialization | 663.6 ms | 584.4 ms | 79.2 ms / 11.9% less |
| Cat, complete page | 872.4 ms | 873.1 ms | +0.7 ms / 0.1% |
| Tokyo, complete page | 706.4 ms | 708.5 ms | +2.1 ms / 0.3% |
| Cat reopened from History, cached | 65.5 ms | 65.8 ms | +0.3 ms / 0.5% |
| First Paris image, setup/decode/dither | 896.9 ms | 895.8 ms | -1.1 ms / 0.1% |
| Paris, complete page including benchmark I/O | 3597.3 ms | 3612.9 ms | +15.6 ms / 0.4% |

Both runs follow the same Cat, Tokyo, cached Cat, Paris sequence. Article
hashes `3c7a9642`, `20090208` and `316a1e7e`, their sizes and decompression
counts match. The Paris bitmap remains `6653db08`, and internal stack
high-water usage remains 292/1024 bytes.

| Font work on hardware | A BEFORE | B AFTER |
| --- | ---: | ---: |
| Large fonts: resident allocation, clearing and initial reads | 87.6 ms | 4.0 ms |
| All fonts: open and file-size lookup | 130.8 ms | 131.6 ms |
| All fonts: resident allocation, clearing and initial reads | 195.9 ms | 112.7 ms |
| All fonts: FAT seek-map construction | 55.0 ms | 55.2 ms |
| All font work combined | 381.7 ms | 299.5 ms |

The large-font row is included in the all-font resident row. Individual
font loads also occur before the final startup font stage, so these totals
must not be added to the consecutive startup-stage timings. The large-font
resident saving is 83.6 ms; small changes in other initialization work leave
79.2 ms of overall improvement. This directly verifies the intended change.
The corresponding emulator saving is larger because its scratch volume
has smaller FAT clusters and slower modeled initial font reads.

There is one run per build. Cold text-page differences remain small and
are similar in magnitude to the emulator results. Paris wrapping adds
2.4 ms and painting adds 13.4 ms, while its archive and image timers are
nearly unchanged. The whole-page timer includes benchmark log I/O, so this
pair does not isolate the source of the extra painting time. There is no
page-load or image-speed gain claimed for this round.

## Emulator measurements

Normal reader, GCC 16.2, current kernel and stock June 2026 archives:

| Work, 32 MB emulator | Before | After |
| --- | ---: | ---: |
| Reader initialization | 1778.59 ms | 1630.52 ms |
| Cat preparation, before painting | 715.14 ms | 715.60 ms |
| Tokyo preparation, before painting | 694.21 ms | 695.08 ms |
| First Paris image, creation through destruction | 796.08 ms | 793.52 ms |

The saved startup, Cat, Tokyo and Paris framebuffers match byte for byte.
On the 16 MB board, initialization takes 1637.20 ms, Tokyo preparation
changes from 698.93 to 699.89 ms, and Paris image processing changes from
798.89 to 796.24 ms. These screens also match the baseline.

The exact apps prepared for the card use the same startup, font, article
and image probes. A BEFORE is an isolated build of `eeebbdd4` (18:41:47);
B AFTER adds this round's font changes (18:42:36).

| Exact A/B pair, 32 MB emulator | A BEFORE | B AFTER |
| --- | ---: | ---: |
| Simple English initialization | 1793.3 ms | 1645.5 ms |
| Each large font: resident setup and initial read | 51.4 ms | 2.0 ms |
| Each large font: FAT seek-map construction | 164.7 ms | 164.7 ms |
| Cat, complete page | 804.3 ms | 805.3 ms |
| Tokyo, complete page | 785.6 ms | 787.5 ms |
| First Paris image, setup/decode/dither | 773.3 ms | 770.9 ms |
| Paris, complete page including benchmark I/O | 3081.0 ms | 3081.1 ms |

Initialization is 147.8 ms shorter (8.2%). The reduction appears in the
initial font reads, as intended. Cold text pages add 1.0-1.9 ms and the
complete Paris time is effectively unchanged. The decoder is unchanged;
its small modeled timing difference follows the changed heap layout.
Article hashes, sizes and decompression-call counts match. The Paris
bitmap remains `6653db08`, and internal stack high-water usage remains
292/1024 bytes. Saved card logs match serial output and the three paired
article framebuffers match byte for byte.

These are modeled guest times. The physical card's larger FAT clusters
make font loading much faster than in the scratch image; the returned
device measurements above establish the actual saving.

## Implementation and checks

The four regular fonts still preload their first 256 records. Each large
font keeps its eight-byte header and existing 2048-slot glyph cache, and
uses that cache for every character. Thus each large font avoids reading
and allocating 256 x 56 bytes, or 43,008 bytes across the three fonts.
Their headers still provide layout metrics immediately, and their FAT seek
maps are still built before the first article.

The resident-record shortcuts now apply only to regular fonts. Large-font
cache fills also retain low character codes; a cached space preserves its
advance width even though it has no drawn bitmap. The font file format,
kernel interface, regular-font preload and cache sizes are unchanged.

`make -C host-tools/zim-reader check` passes. The new BMF test performs
16,416 glyph probes against full synthetic font records, covering both
font sizes, Latin-1, spaces, prefix boundaries, missing glyphs, cache
collisions and fallback from the regular to the large font. It passes
against both the baseline and updated implementation and under Address
Sanitizer and Undefined Behavior Sanitizer. Allocations are filled with
`0xa5` to expose uninitialized reads. Existing WebP, article-link, archive,
FAT-cache and blob-cache checks pass as well.

Both normal applications build with warnings as errors, with the existing
linker RWX-segment warning. A clean normal ZIM rebuild matches the staged
binary. The kernel and mini-libc used by the A/B apps are unchanged.

## Other experiments

| Normal emulator experiment | Startup | Cat preparation | Tokyo preparation |
| --- | ---: | ---: | ---: |
| Baseline | 1778.59 ms | 715.14 ms | 694.21 ms |
| Preload 128 records in every font | 1610.45 ms | 715.18 ms | 702.09 ms |
| Preload 128 only in large fonts | 1707.24 ms | 715.32 ms | 695.18 ms |
| Read only large-font headers, retained | 1630.52 ms | 715.60 ms | 695.08 ms |

Reducing all font preloads caused extra cold Latin-1 reads on Tokyo. Keeping
regular fonts warm and making all large-font glyphs lazy preserves most of
that startup gain with much less cold-page cost.

A separate memory-copy experiment allowed non-overlapping copies to use
the forward path in either address order and batched four aligned word
copies in C33 assembly. With the large-font 128-record variant, it changed
Cat from 715.32 to 718.69 ms, Tokyo from 695.18 to 697.15 ms and the image
from 787.25 to 792.56 ms. It was reverted. Its patch and test source are
preserved in the local artifact directory; no mini-libc change is retained.

The next potential font target is redundant directory lookup: `load_bmf`
opens a font and then calls `file_size` by pathname, whose kernel wrapper
performs `f_stat`. A file-size query using the open handle could avoid that
second lookup, but requires a kernel API addition and separate measurement.
Earlier ELF-loading costs also remain measured only in the emulator, as
described in [round three](PERFORMANCE-ROUND3.md).

## Hardware A/B procedure

Artifacts, hashes, scripts, rejected patches and saved emulator logs are in
`build/performance-2026-09-07-round4/`. The isolated baseline source and
scratch images are in `/tmp/wr-perf4-0907/`. Installation replaces the normal
ZIM app and A/B pair, backs up the previous apps/logs, and preserves the
kernel, fonts, archives, launcher and calibration benchmark.

1. Launch **A BEFORE**, second row left. Select Simple English if needed;
   open **Cat**, then **Tokyo**, then **Cat from History**.
2. Switch to Wikivoyage, open **Paris**, and wait for its photograph.
3. Power off/on and repeat the same sequence with **B AFTER**, second row
   middle. Repeat the pair if desired.
4. Power off and remount the card. Results append to `ab-before.txt` and
   `ab-after.txt`, including startup and per-font timings.

The returned pair follows this sequence, including the cached History
reopen. Raw logs are linked above; local hash verification and parsed
measurements are saved in the artifact directory's `device/` subdirectory.
