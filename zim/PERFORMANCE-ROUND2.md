# Scaler and drawing-buffer performance, 2026-09-07

Baseline: `207044cd`, including all of the first round's hardware-verified
startup and WebP improvements. This round retains the scaler overlay and
clearing drawing rows on demand. The font-width index experiment was slower
on cold pages and is excluded. One returned hardware A/B pair measures
11.3% less reader initialization time and 7.8% less first-photo processing
time. Full Paris loading, including benchmark I/O, takes 1.9% less time.

## Returned hardware results

Raw logs:
[A BEFORE](bench-device-2026-09-07-round2-ab-before.txt) and
[B AFTER](bench-device-2026-09-07-round2-ab-after.txt). The installed normal
reader, both benchmark apps and their icons match the staged SHA-256 hashes;
the kernel is unchanged. The logs identify the exact emulator-tested builds
(17:28:27 and 17:28:29), the same 32 MB board settings and 60 ticks/us timer,
and Simple English at initialization.

| Hardware measurement | A BEFORE | B AFTER | Difference |
| --- | ---: | ---: | ---: |
| Reader initialization | 750.0 ms | 665.1 ms | 84.9 ms / 11.3% less |
| First Paris photo, setup/decode/dither | 1107.6 ms | 1020.7 ms | 86.9 ms / 7.8% less |
| Paris, complete page including benchmark I/O | 3811.1 ms | 3739.1 ms | 72.0 ms / 1.9% less |
| Cat, complete page | 871.6 ms | 872.7 ms | +1.1 ms / 0.1% |
| Tokyo, complete page | 700.4 ms | 707.6 ms | +7.2 ms / 1.0% |

Paris image decoding falls from 1018.9 to 932.0 ms (8.5% less time).
Setup remains 0.4 ms and dithering 88.3 ms. The saved bitmap hash is
`6653db08` in both runs. Cat, Tokyo and Paris article hashes, lengths and
decompression-call counts match; internal stack high-water usage remains
292/1024 bytes.

The absolute initialization and image savings closely match the emulator's
87 ms reductions for this exact pair. Initialization's percentage gain is
larger on hardware because its baseline is much shorter, consistent with
the different FAT cluster sizes discussed below. Text pages show no gain:
Cat is effectively unchanged, while Tokyo is 7.2 ms slower in this sample,
including 4.7 ms more blob time, 2.0 ms more wrapping and 1.0 ms more painting.

These are single runs, not averages. A also opened article 68861 before
Tokyo, whereas B did not, so the full navigation sequences differ. A records
a cached Cat reopen at 63.9 ms; B has no corresponding cached record, so
this pair does not verify the history-reopen performance change. Startup
and the separate first-photo timers still substantiate the retained gains.
Emulator page-transition correctness checks are described below.

## Emulator measurements

Normal reader, GCC 16.2, calibrated 32 MB board, stock June 2026 Simple
English and Wikivoyage archives:

| Work | Before | After | Difference |
| --- | ---: | ---: | ---: |
| Reader initialization | 1863.28 ms | 1778.82 ms | 84.46 ms / 4.5% less |
| First Paris photo, creation through destruction | 1021.16 ms | 937.61 ms | 83.55 ms / 8.2% less |
| Cat preparation, before painting | 714.98 ms | 715.35 ms | +0.37 ms |
| Tokyo preparation, before painting | 694.55 ms | 694.73 ms | +0.18 ms |

The scaler alone takes 938.05 ms for the Paris image. The initialization
saving comes from avoiding the initial drawing-buffer fill. Rows are still
cleared as they are rendered or displayed, so this does not eliminate all
clearing work. It also avoids clearing a long previous article's unused tail
when opening a shorter page. Text rendering adds roughly 2 ms in the paired
benchmark below; there is no measurable cold text-page improvement here.

The normal startup, Cat, Tokyo and Paris framebuffers match the baseline
byte for byte. These are modeled guest times, not host execution times or
predictions of device latency. In particular, the scratch boot filesystem
has 512-byte FAT clusters; the physical card uses 4096-byte clusters and has
much lower font seek-map construction cost.

## Implementation

The WebP row import/export functions and their driving loops share
`ovlwebp` with the coefficient decoder. The overlay grows from 2946 to
4102 bytes, below the existing 5632-byte limit. There are no additional
overlay switches during decoding. All direct callers use C33 long calls,
including the lossless and alpha scaling paths. Pixel arithmetic is unchanged.

The article buffer now tracks the initialized row prefix. Starting a new
article resets that prefix; the first read or write of further rows zeros
them before access. Glyphs, bitmaps, lines, license text, highlights, search
list copies, scrollbar restoration and direct LCD views all ensure their
rows first. The allocation size and kernel interfaces are unchanged.

## Rejected font-width experiment

An optional compact index stored deduplicated 64-character advance-width
pages beside each font. Its original form loaded a whole index on the first
uncached non-Latin width query. A revision read only the directory and needed
512-byte portions, with checksums and fallback to the original glyph path.

| Cold article preparation | Original fonts | Whole index | Paged index |
| --- | ---: | ---: | ---: |
| Cat | 714.98 ms | 909.11 ms | 843.70 ms |
| Tokyo | 694.55 ms | 752.77 ms | 707.63 ms |

Extra file reads and validation outweighed the bitmap reads avoided on
these articles. The deployed reader uses the original fonts and has no
width-index dependency. The rejected patch, generator and binaries are
preserved in the local round-two artifact directory for further experiments.

## Matching device benchmark

`build/performance-2026-09-07-round2/ab/` contains the exact A/B apps, maps,
hashes, source difference and saved emulator logs. A BEFORE was built from
an isolated copy of `207044cd`; B AFTER adds only this round's retained
changes. Both use the same initialization, article and image timers and
the same kernel and mini-libc. `ZIM_BENCH_AB` selects instrumentation and
labels, so building an actual baseline requires the baseline source tree.

| Exact benchmark pair, emulator | A BEFORE | B AFTER |
| --- | ---: | ---: |
| Simple English initialization | 1880.2 ms | 1793.2 ms |
| Cat, complete page | 802.7 ms | 804.4 ms |
| Tokyo, complete page | 783.8 ms | 786.1 ms |
| First Paris photo, setup/decode/dither | 1005.9 ms | 918.9 ms |
| Paris, complete page including benchmark I/O | 3311.6 ms | 3226.0 ms |

The image decode timer falls from 941.2 to 854.2 ms; setup stays at 0.4 ms
and dithering at 64.3 ms. Bitmap hash `6653db08`, article hashes `3c7a9642`
(Cat), `20090208` (Tokyo), and `316a1e7e` (Paris), article lengths and
decompression-call counts match. Stack high-water usage is 292/1024 bytes.
Both apps were launched through their actual labelled entries on writable
images, then their saved logs were checked against serial output. All three
paired article framebuffers match byte for byte.

On the physical card, A BEFORE is second row left and B AFTER second row
middle. Both start with Simple English and log initialization automatically.

1. Power on and launch A BEFORE. Open Cat, then Tokyo. Open Cat again from
   History to exercise the page transition and revisit path.
2. Press Search, tap the globe, select Wikivoyage, and open Paris. Wait for
   its first photograph to appear.
3. Power off and on, launch B AFTER, and repeat exactly the same sequence.
4. Repeat the pair if desired, then power off and remount the card. Results
   append to `ab-before.txt` and `ab-after.txt`; no stopwatch is needed.

Initialization excludes factory boot, kernel boot and loading the app ELF.
Image timers exclude archive extraction and log writing. Whole-page times
include benchmark log I/O, which is present in both builds. Previous-round
logs are backed up when installing this pair.

## Validation and reproduction

The normal ZIM and legacy WikiReader applications build with warnings as
errors; the linker emits its existing RWX load-segment warning. The host
ZIM checks pass, including 157 cached blob reads compared with independent
decompression. A clean full normal rebuild matches the measured binary.

Five C33 image fixtures pass on both 16 MB and 32 MB boards: lossy/lossless,
opaque/alpha, upscaling/downscaling and native size. Output hashes remain
`31045f00`, `7aa254ae`, `21d851d4`, `bafb3be7`, and `fb7242c6`; each half-length
compressed input is rejected.

On both boards, a scripted sequence opens Cat, scrolls it, opens the shorter
A article, and revisits Cat through History. A test build fills the initial
article backing memory with `0xa5`. The shorter-page screens match the
baseline byte for byte with both the normal and poisoned builds. The history
screens have identical overlapping pixels after allowing for a 4-14-row
viewport difference: changed execution timing slightly alters where the
kinetic scroll stops. All three article opens are confirmed by entry probes.
The normal 16 MB Paris image takes 941.34 ms and matches the baseline screen.

Normal profiles use `zim/perf-emulator.py` as described in the first-round
report. Local artifacts are in `build/performance-2026-09-07-round2/` and
scratch images/scripts in `/tmp/wr-perf2-0907/`. The device files are the
normal `zim.app` and the `ab/` apps and icons bound by their SHA256SUMS files.
