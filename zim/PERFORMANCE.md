# Reader performance

The production reader includes the optimizations validated through 2026-09-08.
Startup profiling and its card logs are enabled only when the boot volume
contains `zimlog.on`; the one-shot hardware probe has been retired.

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
  completion polling. Aligned payloads use 32-bit SPI and DMA by default;
  P67 is held at the idle clock level as GPIO during width changes so SPI
  disable/enable does not shift the card's data by one bit. Commands, tokens,
  CRCs and unaligned transfers remain byte-wide. `SD_DMA_BITS=8` selects the
  previous DMA width. `SDRAM_TIMING=STOCK` selects the original memory
  timings for board comparisons. Idle waits and card power-off are documented in
  [power management](BATTERY.md).

## Physical measurements

Full English Wikipedia, February 2026 archive on the 128 GB card, measured on
2026-09-08 with the same instrumented app and byte/word DMA kernels:

| Startup interval | Byte DMA, hardware | Word DMA, hardware | Word DMA, emulator |
| --- | ---: | ---: | ---: |
| File open/allocation map | 5.551875 s | 4.443179 s | 4.094614 s |
| ZIM indexes | 0.063675 s | 0.056535 s | 0.085308 s |
| UI to keyboard | 0.038802 s | 0.036687 s | 0.043587 s |
| Open to keyboard | 5.654354 s | 4.536402 s | 4.223510 s |

Word DMA saves 1.117952 seconds (19.77% of the byte-mode delay). These intervals
exclude the flash loader and kernel startup. The user confirmed the full
kernel works; startup counters show zero read errors, DMA timeouts or fallback.
Both file phases read 7,393 sectors in 232 calls (3,785,216 bytes). The hardware
word build handled every allocation-map payload through 32-bit DMA. Its
file-phase SD time was 4.032076 seconds, including 2.762266 seconds waiting for
DMA; these are nested measurements, not times to add together. Outside the
DMA wait, that file phase still takes 1.680913 seconds. It includes SPI setup,
GPIO clock holding, command/token handling and filesystem work; the current
counters do not isolate those components further.

The matching emulator is 0.312892 seconds optimistic for total startup
(hardware is 7.41% slower). Its UI read count differs with saved state, so use
the matching 7,393-sector file phase when comparing the transfer path. Exact
SHA256s for the validated pair:

- Kernel: `2db8cc043339d7c0ab7cc8b68cbdac920eb35e4b85fea02f5f295a4564dcbe24`
- App: `915da5e4815dbb0262105ee805f8c272334419282ac0825f87d94dea11c8599e`

The raw startup logs, phase comparison and verified artifact identities are
saved locally in `build/wr128/dma32-clock-hold/hardware-success/`. The sector
probe first reproduced the one-bit shift with CPU reads, then confirmed exact
data and CRCs with GPIO clock holding; its old-sequence negative control still
failed. The emulator reproduces those saved buffers and the old kernel's
program-load failure.

Earlier single-run measurements on the 32 MB WikiReader, using the same
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

## SPI width transition fix

The first word-DMA kernels powered off after the splash while loading the app.
Clearing `SPI_INT` before disabling SPI corrected a manual V.2.8 violation
(the physical loader leaves `SPI_INT=0x14`), but did not fix the corrupted reads.

A one-shot probe read the same 512-byte MBR into scratch buffers, compared all
bytes and guard regions, and logged DMA remaining counts and the card's CRC.
It saved diagnostics through the resident byte-DMA kernel and required a
matching byte read after each experiment. The decisive physical comparisons:

| Transfer | Original SPI transition | With GPIO clock hold |
| --- | --- | --- |
| Byte PIO, no payload transition | Exact payload | Exact payload and CRC |
| Byte DMA, no payload transition | Exact payload | Exact payload and CRC |
| Byte PIO, unchanged-width ENA cycle | One-bit-left-shifted payload | Exact payload and CRC |
| Word PIO | One-bit-left-shifted payload | Exact payload and CRC |
| Word DMA at 15 MHz | One-bit-left-shifted payload | Exact payload and CRC |
| Word DMA at 3.75 MHz | One-bit-left-shifted payload | Exact payload and CRC |

All DMA requests completed; the engines were copying an already-shifted
stream. The reference CRC was `dfa5`; successive ENA cycles shifted it to
`bf4b`, `7e97`, and `fd2f`. The first probe also cycled ENA when finishing a
read, shifting CRCs even for the two exact-payload controls. The corrected
probe matched all six payloads and CRCs, while a seventh, deliberately old
transition still shifted the payload and returned CRC `bf4b`.

The production helper waits for SPI idle, saves the interrupt and GPIO state,
and holds clock pin P67 at CPOL as a GPIO output. It clears SPI_INT, disables
SPI, sets the width, re-enables SPI, lets the divider settle, and restores the
pin mux and saved state. This preserves the required disabled configuration
sequence while preventing the observed extra clock. The physical waveform
was not measured; the emulator reproduces the observed response-bit advance
without claiming which electrical edge caused it.

The one-shot app, dump checker, and trace-enabled driver snapshot are archived
locally in `build/wr128/dma32-clock-hold/retired-probe/`, beside the raw
`hardware-probe/` and `hardware-success/` captures. They are outside the
maintained source tree. Production trace hooks are removed; optional startup
profiling remains available for future hardware/model comparisons.

## Validation and history

`make -C emulator test-sd test-dma test-sd-dma-driver` covers the SD model,
the recorded clock-transition fault, and 51 production C33 driver cases
across byte and word builds. The driver tests use a generated 512-byte image
and check payloads, guards, CRC boundaries, recovery, GPIO state, and invalid
SPI control accesses. No attached card or archive is needed.

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
