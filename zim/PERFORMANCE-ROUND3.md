# WebP transforms and brightness-only reconstruction, 2026-09-07

Baseline: `d36ec107`, including both earlier rounds of hardware-verified
improvements. This round moves WebP transforms into internal RAM and omits
unused chroma reconstruction. The returned hardware A/B pair confirms
12.7% less first-photo processing time and 2.5% less full Paris loading
time. Startup and text-page times differ by at most 2.2 ms. New benchmark
probes break down initialization and font loading.

## Returned hardware results

Raw logs: [A BEFORE](bench-device-2026-09-07-round3-ab-before.txt) and
[B AFTER](bench-device-2026-09-07-round3-ab-after.txt). The card's normal
reader, A/B apps and icons match their staged SHA-256 hashes, and the kernel
is unchanged. Both logs identify the exact emulator-tested September 7,
18:06:52 GCC 16.2.0 builds, the same 32 MB board settings and 60 ticks/us
timer, and Simple English at initialization.

| Hardware measurement | A BEFORE | B AFTER | Difference |
| --- | ---: | ---: | ---: |
| First Paris photo, setup/decode/dither | 1027.8 ms | 896.8 ms | 131.0 ms / 12.7% less |
| Paris, complete page including benchmark I/O | 3715.7 ms | 3623.9 ms | 91.8 ms / 2.5% less |
| Reader initialization | 660.1 ms | 662.2 ms | +2.1 ms / 0.3% |
| Cat, complete page | 871.9 ms | 872.5 ms | +0.6 ms / 0.1% |
| Tokyo, complete page | 705.5 ms | 707.7 ms | +2.2 ms / 0.3% |
| Cat reopened from History, cached | 65.3 ms | 65.5 ms | +0.2 ms / 0.3% |

Photo decoding falls from 939.1 to 808.2 ms (13.9% less); setup remains
0.4 ms and dithering changes from 88.3 to 88.2 ms. The 131.0 ms processing
reduction is close to the emulator's 142.4 ms reduction for this exact pair.
Bitmap hash `6653db08` matches. Both runs follow the same Cat, Tokyo, cached
Cat, Paris sequence. Article hashes, lengths and decompression-call counts
match, and internal stack high-water usage stays at 292/1024 bytes.

This is one run per build, not an average. Whole-page results include more
variable work: Paris card-read time increases from 318.4 to 498.2 ms, while
painting outside the timed image operation also becomes shorter. The full
page still improves by 91.8 ms, but that delta cannot be attributed entirely
to the decoder. The separate image timer excludes archive extraction and
benchmark log writing and directly verifies the retained image changes.

## Emulator results

Normal reader, GCC 16.2, existing kernel, stock June 2026 archives:

| First Paris photo, creation through destruction | Time | Reduction from baseline |
| --- | ---: | ---: |
| Baseline, 32 MB board | 937.61 ms | - |
| Transforms in internal RAM | 860.43 ms | 8.2% |
| Transforms plus brightness-only reconstruction | 796.08 ms | 15.1% |
| Combined changes, 16 MB board | 798.89 ms | 15.1% from 941.34 ms |

The second change saves a further 64.35 ms compared with transforms alone.
Normal initialization is 1778.59 ms versus 1778.82 ms before. The startup
framebuffer and Paris framebuffers on both boards match the baseline byte
for byte.

The exact A/B apps prepared for the physical card have identical timing
hooks, including the new font probes. A was built from an isolated copy of
`d36ec107` plus those probes; B adds this round's image changes. Both identify
the build as September 7, 18:06:52, GCC 16.2.0.

| Exact A/B apps, emulator | A BEFORE | B AFTER |
| --- | ---: | ---: |
| Simple English initialization | 1793.4 ms | 1793.3 ms |
| Cat, complete page | 804.2 ms | 804.2 ms |
| Tokyo, complete page | 785.6 ms | 785.6 ms |
| First Paris photo, setup/decode/dither | 915.8 ms | 773.4 ms |
| Paris, complete page including benchmark I/O | 3223.4 ms | 3081.1 ms |

The image timer is 15.5% shorter and the full page timer is 4.4% shorter.
The image decode component drops from 851.1 to 708.8 ms. Bitmap hash
`6653db08`, article hashes `3c7a9642` (Cat), `20090208` (Tokyo) and `316a1e7e`
(Paris), article lengths and decompression-call counts match. Stack high-water
usage remains 292/1024 bytes. The apps were launched through their actual
A/B icons on writable scratch images; saved card logs match the serial
output, and all three paired article framebuffers match byte for byte.

These are modeled guest times, not predictions of device latency. Normal
and instrumented image timers have different boundaries and code layouts;
compare before and after within the same table.

## Implementation and validation

`TransformOne_C`, `TransformTwo_C`, `TransformAC3_C`, `TransformDC_C` and
`TransformWHT_C` join the existing `ovlwebp` overlay. It grows from 4102 to
5220 bytes of the available 5632. Direct callers in `dec.o` use long calls;
the decoder also reaches these routines through function pointers. The
pixel arithmetic and number of overlay switches are unchanged.

For lossy YUV(A) output with `luma_only` enabled, the output hook leaves
U/V unwritten at native size as well as when resizing. With filtering off,
row reconstruction also skips chroma prediction, inverse transforms,
border/cache copies and chroma dithering. Entropy parsing still consumes all
chroma tokens and updates their contexts. Filtering retains the original
reconstruction; RGB output and the lossless RGBA path retain their original
behavior. The existing YUV storage allocation is retained.

`make -C host-tools/zim-reader check` passes, including 157 cached blob
comparisons and a new 168-case decoder equivalence test. The latter compares
brightness and alpha with full reconstruction, checks that omitted U/V
planes remain untouched, and checks that RGB ignores the flag. It covers
lossy and lossless fixtures, native size, up/down scaling, cropping,
filtering and 17-byte incremental input. The same test passes with Address
Sanitizer and Undefined Behavior Sanitizer.

Five larger C33 image fixtures pass on both 16 MB and 32 MB boards, with
original hashes `31045f00`, `7aa254ae`, `21d851d4`, `bafb3be7`, `fb7242c6`.
Half-length compressed inputs are rejected. The normal ZIM and legacy wiki
apps build with warnings as errors; the linker retains its existing RWX
segment warning. A full clean normal rebuild matches the measured binary.

## Startup measurements and next targets

Both A/B apps record five consecutive initialization stages and seven
individual font loads. Font measurements separate opening/file-size lookup,
resident allocation/clearing/reading, and FAT seek-map construction. Samples
are stored in memory; every log write happens after initialization timing
ends. Normal builds compile out these probes.

| Startup stage, Simple English | Emulator A | Hardware A | Hardware B |
| --- | ---: | ---: | ---: |
| Reader initialization before archive search setup | 126.2 ms | 37.0 ms | 37.4 ms |
| Archive/search setup | 230.9 ms | 125.0 ms | 125.0 ms |
| History and temperature settings | 121.3 ms | 41.0 ms | 41.7 ms |
| Intro, logo and initial UI | 336.6 ms | 181.8 ms | 182.3 ms |
| Remaining font loads | 978.1 ms | 275.1 ms | 275.6 ms |

Fonts also load earlier in the other stages, so the individual font rows
overlap this stage table. Across all seven fonts:

| Font work | Emulator A | Hardware A | Hardware B |
| --- | ---: | ---: | ---: |
| Open and file-size lookup | 339.7 ms | 130.5 ms | 131.1 ms |
| Resident allocation, clearing and initial read | 364.7 ms | 195.7 ms | 195.8 ms |
| FAT seek-map construction | 512.5 ms | 55.0 ms | 55.0 ms |
| Total | 1216.9 ms | 381.2 ms | 381.9 ms |

Font setup accounts for about 58% of hardware reader initialization. Its
326.9 ms of opening and resident setup/reads is the largest remaining font
target to investigate. Those resident timings include allocation and
clearing, so they are not measurements of SD transfer time alone. Any
reduction must preserve first-page and non-Latin glyph performance.

Each large font's seek map takes only 16.4 ms on the device, compared with
164.7 ms in the emulator. The scratch FAT volume uses 512-byte clusters;
the physical card uses 4096-byte clusters. The much smaller measured map
cost limits the potential benefit from deferring this work, which could
also move latency into the first non-Latin page. The earlier width-index
experiment already demonstrated that startup work cannot be assessed alone.

Read-only probes also measured the earlier loader path in the normal app:
launcher ELF loading through launcher entry takes about 1384 ms, and reader
ELF loading through reader entry about 1687 ms in this scratch image.
Reader entry to `wikilib_run` is about 0.1 ms, followed by 1779 ms of reader
initialization. These loader intervals exclude time spent waiting at the
launcher and are emulator measurements only. The app's hardware timers
begin after ELF loading; measuring that earlier interval on the device
would require kernel instrumentation.

## Device A/B procedure

Local artifacts, hashes, scripts and emulator logs are in
`build/performance-2026-09-07-round3/`. Scratch images and the isolated
baseline source are in `/tmp/wr-perf3-0907/`. The normal app and A/B apps
are installed together; prior apps and logs are backed up. The kernel,
archives, launcher and calibration benchmark remain unchanged.

1. Launch **A BEFORE**, second row left. Use the globe to select Simple
   English if necessary, open **Cat**, then **Tokyo**, then reopen **Cat
   from History**.
2. Use the globe to select Wikivoyage and open **Paris**. Wait for its
   first photograph, then power off.
3. Power on, launch **B AFTER**, second row middle, and repeat exactly
   the same sequence. Repeat the pair if desired.
4. Power off and remount the card. Timings append to `ab-before.txt` and
   `ab-after.txt`; no stopwatch is needed. Initialization rows record the
   starting archive so matching runs can be compared.

The returned pair follows this sequence, including the cached History
reopen. Raw logs are linked above; local hash verification and parsed
measurements are saved under the artifact directory's `device/` subdirectory.
