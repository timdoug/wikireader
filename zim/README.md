# ZIM reader application

This is a storage-backend fork of `wiki.app`. It reuses the original
WikiReader keyboard, search screens, fonts, history, article renderer, and
scrolling UI while reading a standard ZIM 6 archive directly.

## Card layout

The preferred layout uses an MBR-partitioned card with a small FAT32 first
partition and an exFAT second partition:

```text
partition 1 (FAT32): normal ROOT_IMAGE files
                     kernel.elf = current Grifo kernel
                     wiki.app   = zim.app
                     zim/wiki.nls
partition 2 (exFAT): one or more *.zim files, any names
```

The FAT32 boot partition preserves the mask-ROM and loader's existing boot
contract. The ZIM reader mounts the second partition as FatFs volume `1:` and
scans its root, and the boot volume's `zim/` directory, for `*.zim` files.
exFAT permits archives larger than FAT32's 4 GiB file limit and, for a
contiguous file, avoids walking a large FAT chain at startup.

With more than one archive the keyboard shows the globe key of the original
reader; it opens a list of the archives' own titles and sizes, and the choice
is written to `wiki.ini` on the boot volume so the next boot returns to it.
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
provides fast seek plus 64-bit file size and seek calls.

## Create a card image

On macOS, after building Grifo and the app:

```sh
./zim/make-card-image /tmp/wikireader-zim-card.dmg \
    wikipedia_en-simple_all_nopic_2026-06.zim \
    wikivoyage_en_all_maxi_2026-06.zim
```

Every archive named is copied to the exFAT volume under its own name. The
older `ZIM_FILE OUTPUT.dmg` argument order still works for a single archive.

The script refuses to overwrite an existing image and verifies that the
device it repartitions is the virtual disk image it just attached. It does not
write to a physical SD card.

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
- prefix search without a generated sidecar index
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
  matched against them after percent-decoding
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

Opening `Cat` from the Simple English archive in `wremu` (modeled guest time
from the tap to the first painted page) breaks down as follows. The article is
blob 29 of 84 in a 2 MiB cluster, so 872 KiB of neighbours must be decoded
first; that decode is the floor set by the archive's cluster size.

| Phase | Time |
| --- | ---: |
| Zstandard decode of the cluster prefix | ~2.15 s |
| HTML to text | ~0.15 s |
| word wrap, including the stream height | ~0.15 s |
| clear, render, paint first page | ~0.05 s |

Reopening an article from the same cluster skips the decode entirely, and
reopening one of the last four articles through history skips everything: the
revisit measured 8 ms in `retrieve_article` and a fully painted page 53 ms
after the tap. The decoder's byte copies are post-increment assembly loops; on this core a C
byte loop costs five instructions per byte. Modeled time for the whole load
moves by about 3% between builds with code layout, because the emulated
16-byte instruction queue is sensitive to where hot loops fall, so compare
instruction counts (about 27.6 million for this article) rather than
milliseconds when judging small changes.

The first photograph in the Wikivoyage `Paris` article decodes in about 1.3 s
of modeled time, down from 2.4 s: the WebP rescaler's 64-bit fixed-point
multiplies use the core's `mltu.w` instead of libgcc, VP8 bit reading uses a
log table instead of a software count-leading-zeros and assembles its 24-bit
window from byte loads, the fixed 8- and 16-byte macroblock copies and fills
move whole words, the U and V planes are not rescaled because only luma is
dithered, the in-loop deblocking filter is skipped because one-bit dithering
hides its effect, and Atkinson error diffusion writes two values per pixel
instead of seven read-modify-writes. Output is pixel-identical throughout.
The remaining image cost is VP8 coefficient decoding, luma rescaling,
dithering, and the inverse transform.

Building with `OPT="-O2 -DZIM_TRACE_HASH"` prints the FNV-1a hash and size of
each decoded article on the serial console, which the emulator echoes; compare
it with `zimdump ... blob` output on the host when changing the decoders.
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
article. In the detailed hardware timing model the initial screen appears 16.60
seconds after selection, down from 40.35 seconds when all six images were
decoded eagerly. Lossy images are decoded directly to scaled luma/alpha; the
first photograph's decode falls from 3.64 to 2.44 modeled seconds without an
RGB intermediate buffer.

## Current limits

- External web, telephone, email, and map links are intentionally not
  handled by the offline reader. A link to another article with a fragment
  opens that article at its top.
- CSS and JavaScript are omitted.
- Image-rich articles lazily decode every useful image whose placeholder fits
  in the 512 KiB article stream. Images requested below 80 by 40 pixels,
  unsupported image formats, and compressed image blobs larger than the 512
  KiB work buffer are skipped. Lazy placeholders currently require the HTML
  image to provide both width and height. Current Kiwix archives
  commonly store assets named `.jpg` or `.png` as WebP internally; the decoder
  detects the content rather than relying on the filename suffix.
- A decoded HTML article must fit the 512 KiB article input buffer.
- Legacy LZMA-compressed ZIM clusters are not implemented.
- Search follows the ZIM title ordering and currently applies only the
  MediaWiki first-letter capitalization rule, not full Unicode case folding.

The 64-bit path has been tested in the emulator with a 4.5 GiB exFAT file and
with its live ZIM path-index table relocated to byte 4,300,000,000, forcing a
successful seek and read above 4 GiB. A complete 49 GB English archive has not
yet been exercised, and unusually large individual articles remain subject to
the 512 KiB decoded-article limit.
