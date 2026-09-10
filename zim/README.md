# ZIM reader application

This is a storage-backend fork of `wiki.app`. It reuses the original
WikiReader keyboard, search screens, fonts, history, article renderer, and
scrolling UI while reading a standard ZIM 6 archive directly.

See [performance notes](PERFORMANCE.md) for the retained optimizations,
measured hardware timings, and regression checks.

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

Files stored with a FAT chain can take longer to open, especially full English
Wikipedia. The reader initially reserves 512 bytes for up to 63 fragments,
avoiding a separate sizing pass for ordinary copies. The kernel reads exFAT
allocation metadata in batches of up to 255 sectors using a shared 127.5 KiB buffer;
it never scans the archive contents to build this map. Each walk starts with
an empty cache, and pending filesystem writes are flushed before reading it.
Contiguous runs inside the buffer use sequential aligned word loads, with
geometry and watchdog checks outside the inner loop. Filesystem
calls must be serialized while this optional shared buffer is enabled.
The larger batch passed the hardware startup/page test: 4.071 seconds from
open to keyboard versus 4.536 with 32 sectors and 16 KiB. It reduces the
allocation-map phase from 232 read commands to 30, with no DMA errors or fallback.
Seek-map construction services the watchdog every 128 clusters and bounds the
walk to the volume's cluster count. Older kernels could shut down on the opening
screen if this scan exceeded their 20-second watchdog period.

Keep the scan buffer out of the stack. The current C33 compiler can schedule
a comparison before a large stack-frame adjustment, whose expansion changes
the condition flags before the comparison's branch. With the 16 KiB buffer on
the stack this silently bypassed read-ahead on hardware despite passing host
tests. The buffer lives in BSS; validate performance with the target binary.

On the tested 128 GB card, the February 2026 full English Wikipedia archive
occupies 945,898 clusters in 13 fragments. The opening-screen delay fell from
24 seconds to an estimated 5-6 seconds on hardware. These are manual
observations from before startup logging was added. The corresponding
emulator interval fell from 27.48 to 5.31 seconds, with roughly 0.33 seconds
spent in the seek/map code and the rest largely in SD transfers and driver
work. Each boot still reads about 3.8 MB of allocation metadata.

32-bit DMA (`SD_DMA_BITS=32`) is now the default. The corrected kernel worked
on hardware and measured 4.536 seconds from open to keyboard, down from the
byte-DMA baseline's 5.654 seconds (19.77% less time). Its startup record has
zero read errors, DMA timeouts or fallback. With the original 32-sector batch,
the file phase read 7,393 sectors in 232 calls, all 3,785,216 payload bytes
through word DMA. The matching
emulator predicts 4.224 seconds overall; see [performance notes](PERFORMANCE.md)
for phase measurements and exact kernel/app identities. `SD_DMA_BITS=8` keeps
the previous width available for comparison.

Keeping SPI word-wide across multiple sectors was also tested in the emulator.
It reduces width changes but needs software to realign payloads around SD's
inter-sector tokens and CRCs. The fastest measured candidate retains per-sector
width changes and increases the read batch: 3.981 seconds versus 4.196 for its
32-sector control. See the [batching comparison](PERFORMANCE.md#sd-read-batching-experiment).

The driver holds P67 at the idle clock level as GPIO during SPI width changes;
otherwise disabling/re-enabling SPI advances the card's response by one bit.
Aligned payloads use 32-bit characters and the C33 `swap` instruction restores
byte order in memory. Commands, tokens, CRCs and unaligned payloads remain
byte-wide. Early filesystem setup uses byte DMA, with word mode enabled after
the boot checkpoint. Bounded waits and partial-transfer recovery retain the
byte fallback; overflow or inconsistent counts reject the block. See the
[hardware diagnosis](PERFORMANCE.md#spi-width-transition-fix) for the probe
results and emulator limits.

The `f_lseek` function is 16-byte aligned on C33: unrelated kernel code growth
had moved its 26-byte scan loop across three 16-byte blocks, exceeding the two
buffer slots and adding almost a second. Check target disassembly and timing
after changing that loop; C33 GCC treats this special seek branch as cold and
does not automatically align its loop. `make -C emulator test-sd-dma-driver`
executes the production backend as C33 code with byte-order, unaligned-buffer,
CRC-boundary, timeout, overflow, GPIO restoration and SPI handoff checks in both
width configurations.

With more than one archive the keyboard shows the globe key of the original
reader; it opens a list of the archives' own titles and sizes, and the choice
is written to `zim.ini` on the boot volume so the next boot returns to it.
History entries remember which archive they came from and switch to it when
reopened. A single archive under any name, including the old `wiki.zim`, is
opened directly.

For compatibility, a single FAT32 partition with `zim/*.zim` still works for
archives smaller than 4 GiB.

## Startup diagnostics

Create an empty `zimlog.on` on the FAT32 boot volume to enable startup
measurements. The matching kernel and app must both be installed: the app
uses the new `file_profile` syscall (119). Each boot appends a record to
`zimboot.log`, closing it after startup so a later normal shutdown is not
needed to save the measurement. The log restarts when it reaches 64 KiB.
Remove `zimlog.on` to disable profiling and startup log writes.
With the marker present the kernel also saves `dma.txt` immediately after
filesystem initialization, before loading `init.app`. This early checkpoint
records the DMA width/status and SPI control, receive-mask and interrupt
registers, so an app-load failure need not leave us without a diagnostic.
`payload_bits` is the active width at that checkpoint; `configured_bits`
identifies the width selected for subsequent application reads.

The record identifies the app build, archive and configured DMA width, then
reports three phases in microseconds: `file` covers stat/open and the allocation
map, `indexes` covers opening the ZIM header/indexes, and `ui` ends immediately
after presenting the keyboard. `open_to_keyboard_us` sums those phases;
`app_to_keyboard_us` also includes earlier app initialization, but neither
measures the boot loader or kernel. Logo/font preparation after the first
keyboard frame and log writes are outside these intervals.

Each phase includes requested SD sectors/read calls, time inside SD reads,
successful 32/8-bit DMA payload bytes, time waiting for DMA, bypassed payload
bytes, timeouts and errors. `read_us` is a subset of elapsed time, and
`dma_wait_us` is a subset of read time; do not add them together. A timeout's
partial payload is excluded from successful DMA bytes. The sticky
`dma_disabled` flags also catch a fallback before measurement started.
Counters stay in RAM; formatting and writes happen only after measurement.
Ticks use the 60 MHz MCLK timer, with unsigned differences supporting a
single wrap (individual intervals must be under about 71 seconds).

Compare hardware with the same instrumented binaries: diagnostics can change
both runtime overhead and instruction placement. See [performance notes](PERFORMANCE.md)
for the measured phases and matching kernel/app identities.

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
uses timed event waits (syscall 47) and optional I/O profiling (syscall 119);
update the kernel and reader together.

The kernel defaults to `CARD_POWER=OFF`, removing the SD supply during
deep suspend and reinitialising the card on the next file operation.
During the two-second input and five-second history delays, the reader
uses short CPU HALT waits with the application and touch clocks running.
See [power management](BATTERY.md) for the policy, diagnostics, and measured
card restart latency.

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
  composited into each repaint, plus one photo progress bar per lazy-loading
  batch. The photos within the current four-screen rendering window share a
  fixed total, including archive extraction, WebP decoding, and dithering;
  scrolling into another group starts a new batch. The bar survives viewport
  repaints and restores the covered article pixels when loading pauses or ends
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
  history reuses the completed stream and decoded images; the cache is capped at 2.5 MiB in total

## Memory budget

Production boards (revision V4 and later, or revision 6) configure the SDRAM
controller for 16 MB; only early boards have 32 MB. Addresses past the
configured size alias onto low memory, so the kernel now sets the heap limit
from `ram_size()` instead of the linker script's 32 MB constant, and `wremu`
models the same aliasing once the controller is enabled. Firmware that grows
past the configured size therefore corrupts itself in the emulator exactly as
it would on the device.

On a 16 MB board the heap runs from the end of the program to 15 MB,
about 14.2 MB. Approximate allocations after opening an article are:

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

The large fonts keep only their headers resident and fetch glyphs into
bounded caches. Font fast-seek maps avoid repeated FAT-chain walks.

## Validation

Run the host regression suite with:

```sh
make -C host-tools/zim-reader check
```

The suite covers archive and cluster caching, HTML conversion, article links,
WebP luma/alpha output, font-cache bounds, and large seek-map construction with
a simulated watchdog, batched metadata reads, FAT boundaries, pending writes,
I/O errors, and cyclic chains. `fastseek-watchdog-test exfat-prefix.bin` also
opens archives and verifies fragment-boundary seeks through production FatFs
using a captured exFAT metadata prefix; reads beyond that capture are rejected.
Target-specific code is also checked through full-FLASH emulator boot,
search, Cat/Tokyo article loading,
and the first Paris photograph on 16 MB and 32 MB board configurations.
Tokyo's image-URL regression is checked against the full English archive:
13 escaped asset paths resolve and decode after one URL-decoding pass.
The emulator reproduces the old blank Rainbow Bridge placeholder and the
fixed image, with the preceding photograph pixel-for-pixel unchanged.
The same Tokyo boot-and-scroll check groups the initial five photos under one
monotonic progress bar and the next two under a new bar after a flick. It checks
the drawn bar pixels and preserves image positions through a cached revisit.
The emulator's [profiling commands](../emulator/README.md#profiling-and-timing)
can measure those paths with addresses from the matching `zim.map`.

The physical WikiReader with stock 2009 flash has passed startup, search,
articles, links, scrolling, saved history, and idle/shutdown workflows.
The four performance rounds were verified on a 32 MB device;
[the performance note](PERFORMANCE.md) records the final timings and scope.
The 124 GB full English archive has been exercised in the emulator only.

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

## Sparrow questions

The reader includes an optional [Sparrow](../sparrow/README.md) factual question
mode. Install its generated `sparrow.dat` at the root of the content or boot
volume, type `ask capital of burkina faso`, and tap the answer row. Answers
show their Wikidata lookup steps and link to articles in the selected ZIM.
Sparrow is a first implementation with a real-data sample; full-corpus import
and coverage validation are pending. See the [spec review](../wikibox-spec.md).
