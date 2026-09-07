# Reader performance

The production reader includes the optimizations validated through 2026-09-07.
Normal builds no longer include benchmark apps, timer hooks, or card logs.

## Retained changes

- Startup reads only the needed metadata. Four small fonts preload their
  first 256 records; three large fallback fonts read just their headers and
  fetch glyphs into 2048-slot caches. Header-only loading saves 42 KiB of
  reads and resident allocation. Font fast-seek maps and sector-sized glyph
  fills avoid repeated FAT-chain walks and small card commands.
- Article loading retains a decoded-cluster cache, four finished articles
  within a 2.5 MiB budget, 16 KiB compressed-input reads, and direct HTML
  conversion from cached output. Bank-aware allocation separates hot SDRAM
  buffers; batch copies and compact entropy tables reduce row changes.
- The Zstandard sequence loop and table builder run in A0 RAM. HTML,
  wrapping, Huffman, WebP, and dithering use mutually exclusive IVRAM
  overlays. The WebP overlay occupies 5,220 of its 5,632 bytes; preserve
  linker bounds and long-call flags when changing these paths.
- Lossy WebP reconstruction produces scaled luma/alpha for the one-bit
  display, skips unused chroma work, uses C33 fixed-point multiplies,
  and accelerates transforms, rescaling, and Atkinson dithering. Images
  remain incremental so touch handling runs between decoder checkpoints.
- Grifo retains SDRAM retiming, DMA descriptors in DSTRAM, and bounded DMA
  completion polling. `SDRAM_TIMING=STOCK` selects the original memory
  timings for board comparisons. Idle waits and card power-off are documented in
  [power management](BATTERY.md).

## Physical measurements

Latest single-run measurements on the 32 MB WikiReader, using the same
kernel and card and the June 2026 Simple English and Wikivoyage archives:

| Operation | Time |
| --- | ---: |
| Reader initialization | 584.4 ms |
| Cat, tap to first paint | 873.1 ms |
| Tokyo, tap to first paint | 708.5 ms |
| Cached Cat | 65.8 ms |
| First Paris image processing | 895.8 ms |
| Paris, complete first paint | 3612.9 ms |

Initialization measures entry to the reader through font loading; it excludes
flash boot and the launcher. Image processing excludes article extraction
and layout. These are individual samples, not averages or measurements of
battery energy. The final font change reduced initialization from 663.6 ms
by 79.2 ms (11.9%); article/image differences in that round were small.

The calibrated emulator is useful for comparing work and checking rendering.
Its storage latency and some memory-heavy phases differ from hardware;
confirm timing improvements on a physical device with matching builds.

## Validation and history

`make -C host-tools/zim-reader check` retains archive/cache, HTML/link,
WebP luma/alpha, and font-cache regression coverage. Full-FLASH emulator
runs check startup and Cat/Tokyo/Paris screens on 16 MB and 32 MB boards.
The hardware A/B runs also matched article and image hashes.

The optimization commits are `207044cd`, `d36ec107`, `eeebbdd4`, and
`dee29f20`. Complete reports, raw measurements, and the retired benchmark
harness remain in Git at `dee29f20`, for example:

```sh
git show dee29f20:zim/PERFORMANCE-ROUND4.md
```

For new investigations, use the emulator's maintained `-F`, `-X`, and `-Y`
[profiling options](../emulator/README.md#profiling-and-timing) with the
symbol addresses from the app's matching map file.
