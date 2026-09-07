# ZIM reader application

This is a storage-backend fork of `wiki.app`. It reuses the original
WikiReader keyboard, search screens, fonts, history, article renderer, and
scrolling UI while reading a standard ZIM 6 archive directly.

## Card layout

The preferred layout uses an MBR-partitioned card with a small FAT32 first
partition and an exFAT second partition:

```text
partition 1 (FAT32): normal ROOT_IMAGE files (wiki.app, forth, licences...)
                     kernel.elf = current Grifo kernel
                     init.app   = current launcher
                     init.ini   = launcher entries for zim.app and wiki.app
                     zim.app, zim.ico
                     zim/wiki.nls
partition 2 (exFAT): one or more *.zim files, any names
```

The reader is a peer of the stock `wiki.app`: it has its own launcher entry
and icon (the Kiwix kiwi, from the CC BY-SA 4.0 `Kiwix_logo_v3.svg` on
Wikimedia Commons, thresholded to 64 by 64 one-bit pixels in `zim.xpm`), and
keeps its settings, history, password, and keyboard state in `zim.ini`,
`zim.hst`, `zim.pas`, and `zim.tem`, so both applications can live on one card
without treading on each other's files. With two `init.ini` entries the
launcher shows both icons at power on and a tap starts that application; a
single entry boots straight into it, so delete the `wiki.app` line for an
unattended boot. (Holding a front button while switching on is already taken
by the boot loader in flash, which uses it to start forth or the calculator.)

The FAT32 boot partition preserves the mask-ROM and loader's existing boot
contract. The ZIM reader mounts the second partition as FatFs volume `1:` and
scans its root, and the boot volume's `zim/` directory, for `*.zim` files.
exFAT permits archives larger than FAT32's 4 GiB file limit and, for a
contiguous file, avoids walking a large FAT chain at startup.

With more than one archive the keyboard shows the globe key of the original
reader; it opens a list of the archives' own titles and sizes, and the choice
is written to `zim.ini` on the boot volume so the next boot returns to it.
History entries remember which archive they came from and switch to it when
reopened. A single archive under any name, including the old `wiki.zim`, is
opened directly.

For compatibility, a single FAT32 partition with `zim/*.zim` still works for
archives smaller than 4 GiB.

## Build

```sh
cd zim
make TOOLCHAIN_BIN="$PWD/../host-tools/toolchain-c33/work/install/bin" \
    SIMULATE=NO OPT=-O2
```

The result is `zim/zim.app`. The app includes portable Zstandard and WebP
decoders because both formats must be decoded by the C33 itself; host libraries
cannot be linked into target firmware.

This app requires the current `samo-lib/grifo/grifo.elf`. Its FatFs interface
provides fast seek plus 64-bit file size and seek calls. The reader also
uses timed event waits (syscall 47); update the kernel and reader together.

The kernel defaults to `CARD_POWER=OFF`, removing the SD supply during
suspend and reinitialising the card on the next file operation.
`CARD_POWER=KEEP` opts into lower wake-to-read latency at the cost of powering
the card throughout idle. See [the battery audit](BATTERY.md) for measured
tradeoffs and the remaining power work. The two-second idle debounce is
retained after input failures on the immediate-suspend test build. A new
build uses short CPU HALTs during the idle/history delay, keeping the
application timer and touch clocks running. The normal-reader hardware
typing/article/scroll/idle/shutdown workflow passed; see the audit for scope.

## Create a card image

On macOS, after building Grifo and the app:

```sh
./zim/make-card-image /tmp/wikireader-zim-card.dmg \
    wikipedia_en-simple_all_nopic_2026-06.zim \
    wikivoyage_en_all_maxi_2026-06.zim
```

Every archive named is copied to the exFAT volume under its own name. The
older `ZIM_FILE OUTPUT.dmg` argument order still works for a single archive.
The boot volume receives the stock `ROOT_IMAGE` contents, the current kernel
and launcher, `zim.app` with `zim.ico`, and an `init.ini` listing the reader
first and the stock `wiki.app` second.

The stock app needs a data set in the original format, which this repository
does not contain (the `host-tools/offline-renderer` pipeline that makes one
wants PHP 5, Python 2, and a MediaWiki dump). If you have such a folder from a
WikiReader card or download, `enpedia` or `enquote` for example with
`wiki.idx`, `wiki.fnd`, `wiki.pfx`, `wiki.nls`, and `wiki*.dat` inside, pass it
with `--wiki DIR` and the FAT32 volume grows to hold it:

```sh
./zim/make-card-image /tmp/card.dmg --wiki /path/to/enquote \
    wikivoyage_en_all_maxi_2026-06.zim
```

The folder name must match a line in `ROOT_IMAGE/wiki.inf`; the stock app
lists every folder it finds there, and the script points `wiki.ini` at the
first data set it installs. When `wiki/wiki.app` has been built from this tree
(`cd wiki && make TOOLCHAIN_BIN=...`), the script installs that instead of the
2019 release build in `ROOT_IMAGE`. Verified in the emulator with a Wikiquote
data set: the launcher's WikiReader icon starts the stock app, "love" finds
and opens the Love article, and the kiwi still opens the ZIM reader.

The script refuses to overwrite an existing image and verifies that the
device it repartitions is the virtual disk image it just attached. It does not
write to a physical SD card. Keep finished images somewhere other than `/tmp`:
macOS clears it periodically, and a 124 GB image takes half an hour to
rebuild.

## Emulator

Run the generated image from the repository root with:

```sh
./emulator/wremu -g -S 3 -R -N 3,1000000 \
    -e samo-lib/mbr/flash.rom -c /tmp/wikireader-zim-card.dmg
```

`-N 3,1000000` presses the emulated power switch once. Emulator wall time
depends on the host and timing mode.

With the current 944 MiB test archive and current firmware, the parser is
entered after 0.668 seconds of modeled guest time from a direct Grifo boot on
the dual-volume exFAT image. The equivalent single-volume FAT32 image takes
1.794 seconds because constructing its seek map reads 1,889 FAT sectors.

## Implemented

- ZIM 6 header, path index, and `X/listing/titleOrdered/v1` title index
- several archives per card, chosen from a list of their `M/Title` metadata
  through the original reader's wiki-selection screen, with the choice
  persisted by path
- prefix search without a generated sidecar index; the keyboard types lower
  case, so a multi-word search probes each capitalization of the first four
  words after the first ("United States", then the "United states" redirect)
- article ids with 27 index bits and a 4-bit archive id, enough for the
  27.2 M entries of a full English Wikipedia archive (up to 15 archives)
- redirects, uncompressed clusters, and Zstandard clusters
- standard Grifo/FatFs R0.16 file access, including exFAT, 64-bit file
  positions, and compact fast-seek maps for contiguous or fragmented files
- a four-sector application cache for repeated small, unaligned ZIM index
  reads
- a decoded-cluster cache: the most recently used Zstandard cluster stays
  decoded in memory together with its live decoder, so a blob below the
  decoded frontier is a copy and a blob beyond it just continues the stream.
  Articles are path-ordered, so history navigation and many link follows land
  in the cached cluster. The HTML converter reads straight from the cache
  without an intermediate copy. Zstandard's stable output mode is only
  correct when the buffer holds the whole frame, so the buffer is sized above
  the common 2 MiB Kiwix cluster and re-sized once if the offset table
  reports a larger cluster; clusters above 8 MiB use a windowed fallback
- HTML text extraction with structural breaks for headings and paragraphs,
  plus lists and linearized tables
- inline WebP photographs, maps, and drawings, scaled for the display,
  composited onto white, and Atkinson-dithered into the native one-bit article
  bitmap format
- lazy image decoding and bounded article pre-rendering: the first visible
  image is loaded before presentation, while later images are decoded only as
  scrolling approaches them
- a stable full-article scrollbar derived from the packed stream layout and
  composited into each repaint, plus per-image progress for archive
  extraction, WebP decoding, and dithering
- continuous image work that services pending drags at decoder checkpoints,
  with stale consecutive motion samples collapsed to the newest position
  without restarting decompression or decoding
- underlined internal article links using the native WikiReader hit-testing
  and history flow; targets are resolved on tap, including relative paths,
  percent-encoded UTF-8, and redirects, with query/fragment suffixes ignored
- same-page links: footnote markers, "see section" links, and tables of
  contents scroll to their target. Element ids are recorded as anchors while
  wrapping, together with the line they start, and a tapped `#fragment` is
  matched against them after percent-decoding. A link to another article
  that carries a fragment opens that article scrolled to the fragment
- navigation and editing chrome is dropped: navboxes, edit-section links,
  jump links, category footers, printfooters, sister-site boxes, empty
  elements, and anything styled `display:none`. References, infoboxes,
  hatnotes, and tables of contents are kept. For `Cat` this halves the line
  count while keeping every paragraph and citation
- UTF-8/entity handling and font-metric word wrapping into the existing
  WikiReader article stream
- the original WikiReader top-edge progress bar, driven by actual article
  lookup, cluster decompression, conversion, and wrapping milestones
- draw-buffer clearing limited to the rows the previous article touched
  instead of the whole 3.8 MiB off-screen buffer
- a finished-article cache: leaving an article keeps its wrapped stream, with
  whatever images were decoded into it, plus the text its link and image
  tables point into. Returning to one of the last four articles through
  history repaints in about 50 ms instead of decoding, converting, wrapping,
  and decoding images again; the cache is capped at 2.5 MiB in total

## Memory budget

Production boards (revision V4 and later, or revision 6) configure the SDRAM
controller for 16 MB; only early boards have 32 MB. Addresses past the
configured size alias onto low memory, so the kernel now sets the heap limit
from `ram_size()` instead of the linker script's 32 MB constant, and `wremu`
models the same aliasing once the controller is enabled. Firmware that grows
past the configured size therefore corrupts itself in the emulator exactly as
it would on the device.

With `ZIM_TRACE_HASH` the app prints the allocator's block list after each
article and image. On a 16 MB board the heap runs from the end of the program
to 15 MB, about 14.2 MB, and after opening an article the app holds:

| Allocation | Size |
| --- | ---: |
| off-screen article draw buffer | 4.0 MB |
| decoded-cluster cache with its Zstandard state | 2.9 MB |
| article stream, raw, and text buffers | 1.5 MB |
| fonts: four small fonts resident, three large fonts as 2048-glyph caches | 0.7 MB |
| per-line render info and everything else | 0.7 MB |
| **allocated** | **9.3 MB** |
| finished-article cache, grows on use | up to 2.5 MB |
| free | 4.9 MB before the article cache fills |

The three CJK "all" fonts used to be reserved at their full 3.6 MB file size
each and filled lazily; that alone put the app 3 MB past the end of a 16 MB
board, masked until now by the emulator's flat 32 MB window.

## Article load cost

Opening `Cat` from the Simple English archive takes 1.07 s in `wremu`
(calibrated model, `-Y` window between `retrieve_article` and
`render_article_with_pcf`), down from 2.86 s for the morning's firmware and
1.94 s for the state the device measured at 1.85 s (tap to painted page,
`ZIM_BENCH` build, 2026-09-05). The article is blob 29 of 84 in a 2 MiB cluster,
so 872 KiB of neighbours must be decoded first; that decode is the floor set
by the archive's cluster size. Instruction counts moved 25.1 M to 22.4 M, so
most of the gain is memory behaviour, not instruction count. The phase table
below is from the earlier, manual-only emulator model (before it was
calibrated to the device), so its absolute times are low by about a third;
the proportions hold:

| Phase | Before | After |
| --- | ---: | ---: |
| Zstandard decode of the cluster prefix | 1.20 s | 0.80 s |
| kernel, mostly the card read polling loop | 0.11 s | 0.13 s |
| HTML to text | 0.21 s | 0.18 s |
| word wrap, including the stream height | 0.08 s | 0.07 s |
| whole window, tap to first paint | 1.94 s | 1.25 s |

(Phase figures are per-function cycle totals from the `-F` profile, the
Zstandard row summing the sequence, Huffman, FSE-table and copy routines;
the window also contains time the CPU spends waiting on the card.) Under
the calibrated model with the A0 RAM decoder and the IVRAM overlays the
split of the 1.07 s is roughly: Zstandard 0.50 s, HTML to text 0.15 s,
card and kernel 0.14 s, wrap 0.07 s, the rest in the Huffman literal
decoder and the paint; only 11% of the load's cycles are still spent
waiting for instruction fetch.

The S1C33E07 has no cache. Every instruction fetch and data access goes to
the SDRAM controller, which keeps one open row per bank and pays a precharge
and activate whenever an access in a bank moves to another 1 KiB row. On the
16 MB boards a bank is a contiguous 4 MB quarter of memory, so the decoder
used to alternate between rows of one bank for the compressed input, the
entropy tables, the literals, and the first 445 KiB of the output, and every
byte-wise copy switched rows twice per byte. The emulator now attributes SDRAM
row activations to instructions (`-F` file, `--- window sdram` lines), which
is what found the following:

- `zim_alloc_bank_local()` places the decoded-cluster buffer inside one SDRAM
  bank (bank 2 on a 16 MB board), away from the tables, literals and input
  in bank 1, the code in bank 0, and the stack in bank 3. The kernel's
  first-fit allocator is steered with temporary fillers.
- Zstandard literals stay in the decoder context's extra buffer instead of
  the output buffer whenever they fit (64 KiB per block).
- Literal and match copies load a whole batch of 8, 16 or 32 bytes into
  registers before storing it, so a copy costs two row changes rather than
  two per byte; the batch width follows the alignment of source and
  destination and, for matches, the offset. Copies over-run to the end of
  the batch, which the fast path's 32-byte slack allows, and a zero-length
  literal run (half the sequences) is skipped outright.
- The sequence decoder refills its 32-bit bit container only when the next
  field would not fit, instead of after every field, and bit masks are
  computed with a shift instead of read from a table in SDRAM.
- The HTML converter's byte scanners compare against registers instead of a
  class table in the code's bank, tag names are classified through packed
  two-byte keys, and the wrapper keeps its ASCII width table on the stack.
  Output is byte-identical for 118 reference articles.
- The kernel now retimes the SDRAM controller at boot (`SDRAM_TIMING=FAST`,
  `samo-lib/grifo/src/sdram.c`): the flash loader programs the maximum
  tRP/tRAS/tRC of 4/8/15 clocks and a refresh every 141 clocks; the board's
  EM48AM1684VTD-75 needs 2/3/4 at 60 MHz and a refresh every 7.8 us, and the
  kernel programs 2/4/6 and 289 clocks from A0 RAM. A row change drops from
  about 19 to 7 clocks, and the refresh no longer costs 13% of the bus and a
  reactivation of every bank every 141 clocks. Without this, the software
  changes alone give 1.56 s. `SDRAM_TIMING=STOCK` builds a kernel that leaves
  the loader's values. The kernel's suspend code, which the event loop enters
  whenever the reader idles, rewrites the refresh register on every wake; it
  now restores the retimed interval (the first retimed kernel lost it after
  the first idle, which the benchmark build's register line caught).
- The SD DMA backend keeps its IDMA descriptor table and dummy transmit byte
  in DSTRAM, and polls for completion from a loop that fits the instruction
  queue, so a block no longer alternates between SDRAM rows for every byte.
  A 512-byte block payload falls from 52,300 to 22,900 modeled cycles; the
  archive open and title index read at start-up (app start to keyboard) go
  from 457 ms to 294 ms.

Measured on the Wikivoyage archive under the calibrated model, the first
`Paris` photograph decodes in 1.57 s instead of 1.86 s with the same
instruction count, entirely from the SDRAM retiming.

Three more changes followed the device's own measurements (the `ZIM_BENCH`
build, below):

- The Zstandard sequence loop and FSE table builder run from the chip's
  zero-wait A0 RAM. The C33 has no instruction cache and the calibrated
  model showed the load spending 39% of its cycles waiting for code from
  SDRAM. The kernel leaves A0 RAM from 0xc00 to 0x1fc0 (5056 bytes) to
  applications (`grifo.lds` asserts its own relocated code stays below,
  `application.lds` defines the `fastram` region and the `.fastcode`
  output section), the kernel's ELF loader copies the section in like any
  other, and its SD reads into internal RAM use the byte-at-a-time path
  because HSDMA into A0 RAM is not something the board has been seen to
  do. The two functions take 4340 bytes; `zstddeclib.c` is compiled with
  long calls so they can reach the rest. `Cat` fell from 1.82 s to 1.30 s
  in the model and from 1.85 s to 1.39 s on the device (2026-09-06,
  `bench-device-2026-09-06.txt`). A0 RAM fetch measured exactly one cycle
  per instruction on the device; the remaining 12% gap in the decoder
  phase is in its store-then-load traffic, which the model still
  underprices.
- Glyph misses no longer cost a cluster-chain walk and a card command each.
  The font files now get FatFs fast-seek maps at load (a seek without one
  followed the FAT chain from the start of a 3.6 MB font: 83% of a
  CJK-heavy article's load was the kernel doing that), and a miss reads
  the whole sectors its record lies in and keeps every record in them,
  since the card delivers sectors and charges about 1.2 ms per command.
  `Tokyo` (Simple English, Japanese in the first line) went from 8.05 s to
  1.14 s tap to paint in the model and takes 1.03 s on the device; a Latin
  article's first page after a cold boot,
  which the device timed at 1.8 s of glyph loading, is helped the same way.
- Cluster input is read in 16 KiB slices instead of 4 KiB, six card
  commands instead of 24 for `Cat`'s prefix, saving about 50 ms per load.
- The HTML converter, the word wrapper, and the Huffman literal decoder
  run as overlays in the LCD controller's 5632-byte window buffer in
  IVRAM, which the device fetches from as freely as A0 RAM (measured
  2026-09-06). Each is linked to run there with the linker's `OVERLAY`
  command and stored in SDRAM; `zim_overlay.c` copies one in before its
  phase (about 40 us), and the phase entry that follows another overlay
  copies again. C33 calls and jumps are PC-relative even in their long
  forms, so code must be linked for the address it runs at; the kernel's
  loader places sections by address, so the reader's Makefile moves each
  overlay section's address to its load address after linking. The
  converter's once-per-element paths moved out of line to fit. `Cat` fell
  from 1.30 s to 1.07 s in the model; the benchmark build's tap-to-paint
  line from 1353 to 1121 ms, `Tokyo` from 1141 to 980 ms. On the device:
  `Cat` 1391 to 1245 ms, `Tokyo` 1030 to 920 ms. The model runs the
  overlay and A0 RAM phases about 15% fast (zstd 687 ms measured against
  587, HTML 251 against 210): their fetch is right, so the gap is in
  data traffic the micro-benchmarks do not exercise.

A third round on 2026-09-06 (evening) took the model's `Cat` from 1073 to
761 ms, with all eight reference articles, their converted text and their
wrapped streams hashing identically before and after; the device has not
measured this build yet. What it found and changed, in the order of
value:

- The three FSE table entries were read field by field in an interleaved
  order, nine SDRAM row changes per sequence. They are now one word each
  in a compact form (`ZSTD_c33_packEntry`: 9-bit next state, bit counts,
  a 14-bit base with the four large bases marked and recomputed) built
  straight into the IVRAM window buffer, which is free between a block's
  literals and its sequences; the context keeps a master copy for blocks
  that repeat a table. The sequence state itself is a static in A0 RAM.
- The sequence loop and the Huffman literal decoder run on a private 1 KB
  stack in DSTRAM (`ZSTD_c33_callOnDstramStack`), so the compiler's spills
  cost a cycle instead of an SDRAM data-queue fill each. The emulator
  reports the deepest point reached (180 bytes for `Water`); interrupts
  taken during the loop use the same stack.
- Bank placement is explicit: the cluster output and the literal scratch
  buffer in bank 2, the decoder context and the compressed input in bank
  1, the text buffer in bank 1, the stack in bank 3; the FSE builder keeps
  its per-symbol arrays and spread buffer on the stack.
- Copies from a source that is not word-aligned with the destination use
  four word loads and shifts per 16 bytes instead of byte batches.
- The converter compares names as packed words against constants and never
  reads a literal from `.rodata` (each byte of the old loop changed row
  three times), takes href, src and the image dimensions in its single
  attribute pass, and copies records inline; the wrapper keeps its
  word-break table on the stack; the progress bar extends from its last
  end instead of redrawing from the left edge (5% of the load).
- The emulator gained unaliased profile buckets (internal-RAM code had
  been sharing buckets with the kernel), per-row and row-pair activation
  histograms, a row trace, repeating windows and probe caller lists,
  which is how the above were found.

One bug came out of this round and is worth recording. Packing a table
entry into a word leaves 14 bits for the base value; larger bases are
marked and recomputed from the extra-bit count, and the rule is not the
same for the three tables: literal length is `1 << bits`, match length
`(1 << bits) + 3`, offset `(1 << bits) - 3`. The match-length rule was
missing, so a match of 16387 bytes or more decoded as 16383 bytes. Only
articles containing such a match were damaged, and none of the eight in
the hash suite did; `Japanese Bobtail` decoded to `bb1f39d8` against the
archive's `83d60b5f` and displayed as scrambled words. The trace build
now unpacks each entry and compares it with the table it came from, and
the suite covers that article.

Measured on the device with the fix (2026-09-06 night), tap to painted
page: `Cat` 1121 ms, `Tokyo` 747 ms, `Japanese Bobtail` 1280 ms, against
1245 and 920 ms for the first two at midday. Every phase runs 25 to 40%
slower than the model there, wider than the 15% residue seen earlier, and
the first thing to check is bank placement: the choices here name banks by
absolute index and were tuned on the emulator's 16 MB board with 4 MB
banks, while the device has 8 MB banks.

Tried and reverted: 64 KiB input slices (the decoder runs on to the end
of each slice, 100 ms of unread cluster for `Cat`, and the stable output
buffer forbids bounding the output instead); keeping the card powered
across a suspend (the auto-off timer restarts on every wake).

Reopening an article from the same cluster skips the decode entirely, and
reopening one of the last four articles through history skips everything:
the revisit measured 8 ms in `retrieve_article` and a fully painted page 53 ms
after the tap. Modeled time for the whole load moves by about 3% between
builds with code layout, because the emulated 16-byte instruction queue is
sensitive to where hot loops fall, so compare instruction counts and the
window's row-activation count rather than milliseconds when judging small
changes.

The remaining decoder cost is about 37% of the window: 179 instructions
per sequence (59.6 K sequences for `Cat`, copies included) at a CPI of 1.6,
of which the copies are about 100 cycles, the bit reads and reload checks
about 50 instructions, and the table entries three loads. Most of the
remaining row activations are re-openings after each auto-refresh closes
every bank, at about two clocks each.

Building with `OPT="-O2 -DZIM_TRACE_HASH"` prints the FNV-1a hash and size of
each decoded article on the serial console, which the emulator echoes; compare
it with `zimdump ... blob` output on the host when changing the decoders (eight
articles including the 967 KB `Water` were checked after these changes).
`host-tools/zim-reader/make check` verifies the cached, continued, truncated,
and zero-copy blob paths against an independent whole-cluster decode.

The repository test archive
`wikipedia_en-simple_all_nopic_2026-06.zim` has 401,965 directory entries,
396,632 searchable titles, and 3,764 clusters. The target app has been booted
against it in `wremu`; searching for `CAT`, opening `Cat`, and rendering the
article all succeed.

Image support has also been exercised end to end with
`wikivoyage_en_all_maxi_2026-06.zim`: the emulator searched for and opened
Paris and displayed a 226-pixel-wide dithered photograph inline with the
article. Lossy images are decoded directly to scaled luma/alpha; the WebP
rescaler's 64-bit fixed-point multiplies use the core's `mltu.w`, VP8 bit
reading uses a log table instead of a software count-leading-zeros, the U and
V planes are not rescaled because only luma is dithered, the in-loop
deblocking filter is skipped because one-bit dithering hides its effect, and
Atkinson error diffusion writes two values per pixel. The remaining image
cost is VP8 coefficient decoding, luma rescaling, dithering, and the inverse
transform.

## Timing on the device

There is no serial cable yet, so the reader can time itself and leave the
results on the card, the way the kernel leaves `dma.txt`. Build the
benchmark variant and install it beside the normal reader as a second
launcher entry (the inverted kiwi):

```sh
cd zim && rm -f build/*.o zim.app
make TOOLCHAIN_BIN=... ZIM_BENCH=YES
cp zim.app /Volumes/WRBOOT*/zimbench.app; cp zimbench.ico /Volumes/WRBOOT*/
printf '%s\n' 'zimbench.ico : zimbench.app started-from-init' >> /Volumes/WRBOOT*/init.ini
rm -f build/*.o zim.app && make TOOLCHAIN_BIN=...     # back to the normal app
```

At start-up the benchmark app runs a set of micro-benchmarks (about two
seconds), and after every article load it records the load by phase. Each
result is one line appended to `bench.txt` on the boot volume and printed on
the serial console:

```text
bench start: sdram ctl 0x00001352 ref 0x03ff0120 app 0x8000000b, timer 60 ticks/us
bench cpu-loop          3000000 ops     200.0 ms      4.0 cyc/op
bench pair-row-change    100000 ops      37.6 ms     22.6 cyc/op
bench card-256k             512 ops     197.9 ms  23199.6 cyc/op
article 70197 total 1326.5 blob 918.5 (card 83.9 96K 24, zstd 813.5 24) html 240.0 wrap 129.0 paint 38.9 ms, 130154 29853 24235 bytes
```

The first line shows the SDRAM controller registers the kernel is actually
running with. The micro-benchmarks isolate one thing each: `cpu-loop` a
register-only loop (the clock and branch cost), `fetch-1k` straight-line
code (instruction fetch), `read-`/`write-` sequential SDRAM access by words
and bytes, `pair-same-row`, `pair-row-change` and `pair-two-banks`
alternating word reads with nothing, a row change, or a bank switch between
them, the three copies of 512 KiB, and the card: 256 KiB sequential (twice)
and 64 reads of 4 KiB a megabyte apart. `cyc/op` is timer ticks per
operation, which are MCLK cycles. Article lines give tap-to-paint by phase;
the blob phase is split into the card reads and the Zstandard calls it
contains.

Run the same app in the emulator on a scratch copy of the card image
without `-R` (a rejected write of `bench.txt` on a read-only image leaves the
guest's file system unable to open the fonts) and the lines appear on its
standard output and in the image's `bench.txt`. Compare with

```sh
zim/bench-compare device-bench.txt emulator-bench.txt
```

which prints both times for every matching line and their ratio. The
emulator's timing parameters were fitted to the device's file this way on
2026-09-05 (`emulator/tools/fit_model.py`, `emulator/README.md`
"Calibration"); the file is kept as `bench-device-2026-09-05.txt`. Every
micro-benchmark now agrees within 10% and the `Cat` load within 8%.

## Hardware status

Run on a real WikiReader with its stock 2009 flash on 2026-09-05, from an
8 GB card written with `dd` from the dual-archive image: the factory loader
loads the kernel, the launcher shows both icons, the reader searches, opens
articles, follows links, coasts after a flick, and keeps history across a
power cycle. Two things only the hardware caught, both fixed: the suspend
code's saved clock registers had been spilled to SDRAM (gcc 16), and the SD
DMA backend slept in HALT for a completion interrupt that never woke the
core. The 124 GB full English archive has not yet been tried on a card.

The kernel's SDRAM retiming and the DMA descriptor move to DSTRAM described
under "Article load cost" ran on the same device later on 2026-09-05, and
the A0 RAM decoder, fast-seek fonts, and the loader's byte-path read into
internal RAM on 2026-09-06: it boots, searches, and opens articles, no
`dma.txt` fallback note appeared, and its own `ZIM_BENCH` timings are in
`bench-device-2026-09-05.txt` and `bench-device-2026-09-06.txt` (`Cat`
1.85 s then 1.39 s, `Tokyo` 1.03 s). The
retiming uses data-sheet values with margin; a board that misbehaves with it
(garbled screen, hangs, wrong articles) should be given a
`SDRAM_TIMING=STOCK` kernel, and the DMA change falls back to byte-at-a-time
SPI through the existing timeout path if the engines do not complete.

## Current limits

- External web, telephone, email, and map links are intentionally not
  handled by the offline reader.
- CSS and JavaScript are omitted.
- Image-rich articles lazily decode every useful image whose placeholder fits
  in the 512 KiB article stream. Images requested below 80 by 40 pixels,
  unsupported image formats, and compressed image blobs larger than the 512
  KiB work buffer are skipped. Lazy placeholders currently require the HTML
  image to provide both width and height. Current Kiwix archives
  commonly store assets named `.jpg` or `.png` as WebP internally; the decoder
  detects the content rather than relying on the filename suffix.
- A decoded HTML article is converted straight out of the decoded-cluster
  cache, so its size is bounded by the cluster (2 MiB in Kiwix archives),
  while the converted text and the wrapped article stream must each fit
  512 KiB; the largest full English articles measured (United States,
  1.3 MB of HTML) produce about 375 KB of text and 250 KB of stream.
- An article keeps at most 8,192 internal links and 8,192 section anchors;
  United States has 3,948 links.
- Legacy LZMA-compressed ZIM clusters are not implemented.
- Search follows the ZIM title ordering with capitalization variants of the
  first words only, not full Unicode case folding.

The 64-bit path has been tested in the emulator with a 4.5 GiB exFAT file and
with its live ZIM path-index table relocated to byte 4,300,000,000, forcing a
successful seek and read above 4 GiB, and with the complete 124 GB
`wikipedia_en_all_maxi_2026-02.zim` (27.2 M entries, 216 k clusters): search,
article and image loading, history, and scrolling work on a 124 GB card image.
