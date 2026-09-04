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

The result is `zim/zim.app`. The included Zstandard decoder accounts for about
71 KiB of target code.

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
- HTML text extraction with structural breaks for headings and paragraphs,
  plus lists and linearized tables
- UTF-8/entity handling and font-metric word wrapping into the existing
  WikiReader article stream

The repository test archive
`wikipedia_en-simple_all_nopic_2026-06.zim` has 401,965 directory entries,
396,632 searchable titles, and 3,764 clusters. The target app has been booted
against it in `wremu`; searching for `CAT`, opening `Cat`, and rendering the
article all succeed.

## Current limits

- Article links are displayed as text but are not yet clickable.
- Images, CSS, and JavaScript are omitted; a `nopic` archive is the appropriate
  input for the current renderer.
- A decoded HTML article must fit the 512 KiB article input buffer.
- Legacy LZMA-compressed ZIM clusters are not implemented.
- Search follows the ZIM title ordering and currently applies only the
  MediaWiki first-letter capitalization rule, not full Unicode case folding.

The 64-bit path has been tested in the emulator with a 4.5 GiB exFAT file and
with its live ZIM path-index table relocated to byte 4,300,000,000, forcing a
successful seek and read above 4 GiB. A complete 49 GB English archive has not
yet been exercised, and unusually large individual articles remain subject to
the 512 KiB decoded-article limit.
