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
partition 2 (exFAT): wiki.zim
```

The FAT32 boot partition preserves the mask-ROM and loader's existing boot
contract. The ZIM reader mounts the second partition as FatFs volume `1:` and
opens `1:/wiki.zim`. exFAT permits archives larger than FAT32's 4 GiB file
limit and, for a contiguous file, avoids walking a large FAT chain at startup.

For compatibility, a single FAT32 partition with `zim/wiki.zim` still works
for archives smaller than 4 GiB.

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
./zim/make-card-image \
    wikipedia_en-simple_all_nopic_2026-06.zim \
    /tmp/wikireader-zim-card.dmg
```

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
- UTF-8/entity handling and font-metric word wrapping into the existing
  WikiReader article stream
- the original WikiReader top-edge progress bar, driven by actual article
  lookup, cluster decompression, conversion, and wrapping milestones
- draw-buffer clearing limited to the rows the previous article touched
  instead of the whole 3.8 MiB off-screen buffer

## Article load cost

Opening `Cat` from the Simple English archive in `wremu` (modeled guest time
from the tap to the first painted page) breaks down as follows. The article is
blob 29 of 84 in a 2 MiB cluster, so 872 KiB of neighbours must be decoded
first; that decode is the floor set by the archive's cluster size.

| Phase | Time |
| --- | ---: |
| Zstandard decode of the cluster prefix | ~2.14 s |
| HTML to text | ~0.15 s |
| word wrap, including the stream height | ~0.15 s |
| clear, render, paint first page | ~0.05 s |

Reopening an article from the same cluster skips the decode entirely.

The first photograph in the Wikivoyage `Paris` article decodes in about 1.6 s
of modeled time, down from 2.4 s: the WebP rescaler's 64-bit fixed-point
multiplies use the core's `mltu.w` instead of libgcc, VP8 bit reading uses a
log table instead of a software count-leading-zeros, and the in-loop
deblocking filter is skipped because one-bit dithering hides its effect. The
remaining image cost is VP8 coefficient decoding, the rescaler, and dithering.
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

- Same-page fragment scrolling and external web, telephone, email, and map
  links are intentionally not handled by the offline reader.
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
