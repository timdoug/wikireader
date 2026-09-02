# ZIM reader application

This is a storage-backend fork of `wiki.app`. It reuses the original
WikiReader keyboard, search screens, fonts, history, article renderer, and
scrolling UI while reading a standard ZIM 6 archive directly.

## Card layout

Install `zim.app` as `wiki.app` and place the archive and English UI strings
at:

```text
zim/wiki.zim
zim/wiki.nls
```

`XML-Licenses/en/wiki.nls` is suitable for the second file. The fixed 8.3
names are intentional: the application's direct FAT32 mapper does not depend
on long-filename support.

## Build

```sh
cd zim
make TOOLCHAIN_BIN="$PWD/../host-tools/toolchain-c33/work/install/bin" \
    SIMULATE=NO OPT=-O2
```

The result is `zim/zim.app`. The current GCC 16.2 build is about 224 KiB on
disk. The included Zstandard decoder accounts for about 71 KiB of target
code.

## Emulator

Once a FAT32 card image contains `zim.app` as `wiki.app` and the files from
the card layout above, run it from the repository root with:

```sh
./emulator/wremu -g -S 3 -R -N 3,1000000 \
    -e samo-lib/mbr/flash.rom -c /tmp/wikireader-zim-card.dmg
```

`-N 3,1000000` presses the emulated power switch once. Initial archive setup
takes roughly ten seconds in the emulator before the keyboard appears.

## Implemented

- ZIM 6 header, path index, and `X/listing/titleOrdered/v1` title index
- prefix search without a generated sidecar index
- redirects, uncompressed clusters, and Zstandard clusters
- fragmented FAT32 files through a one-time cluster map and direct sector I/O
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
- The Grifo/FAT interfaces and this mapper use 32-bit sizes, so one archive
  must be smaller than 4 GiB.
- A decoded HTML article must fit the 512 KiB article input buffer.
- Legacy LZMA-compressed ZIM clusters are not implemented.
- Search follows the ZIM title ordering and currently applies only the
  MediaWiki first-letter capitalization rule, not full Unicode case folding.
