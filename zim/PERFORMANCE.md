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

## SD read batching experiment

On 2026-09-08, production C33 code was tested against the captured 128 GB card
metadata, with the same ZIM app and fonts. These are **emulator measurements**;
the retained larger-batch kernel subsequently passed the physical startup/page
test. Its hardware measurements follow the comparison below.

| SD transport | Read batch | Open to keyboard | File-phase commands | File-phase width changes |
| --- | ---: | ---: | ---: | ---: |
| Hardware-tested payload driver | 32 sectors | 4.196104 s | 232 | 14,786 |
| Entire protocol word-wide | 32 sectors | 4.484702 s | 232 | 0 |
| Byte commands, word-wide multi-sector reads | 32 sectors | 4.346828 s | 232 | 464 |
| Byte commands, word-wide multi-sector reads | 255 sectors | 4.115898 s | 30 | 60 |
| Retained payload driver, larger batches | 255 sectors | 3.981498 s | 30 | 14,792 |

Width-change counts follow the driver's two changes per payload or per read
command; they are not measurements of physical clock edges. SD places a CRC
and byte-oriented token between sectors even inside a multi-block command.
Keeping 32-bit characters across these boundaries requires payload realignment;
its CPU cost outweighed the saved width changes in this model. All variants
opened the archive with zero read errors, DMA timeouts or fallback. The protocol
experiments also passed 16 C33 tests covering reads, writes, token alignment,
power cycling, FLASH handoff and DMA recovery.

Only the larger read-ahead batch is retained. It saves 0.214606 s (5.1%) in this
comparison, costs another 111.5 KiB of BSS, and reads 7,396 rather than 7,393
sectors for this archive. Fragmented chains can over-read part of a batch at
each extent; the cache test bounds this and verifies FAT-boundary clipping.
The linked kernel ends at 0x1003ac00, below the 256 KiB kernel-region limit.
The historical 4.223510 s control below used a different boot-volume fixture; its
file/map phase agrees with this experiment's 4.094615 s control.

The physical test measured **4.071086 s** from open to keyboard, down from
4.536402 s: 0.465316 s saved (10.26%). File/map time fell from 4.443179 s to
3.977882 s, with the expected 30 commands and 7,396 sectors. All payloads used
word DMA, with zero read errors, DMA timeouts, overflow or fallback. DMA wait
was essentially unchanged: 2.762266 s before, 2.763685 s after. This points to
the avoided command/response and inter-transfer overhead as the saving. The
model predicted a 0.214606 s saving and was 0.089588 s faster than the new
hardware result. One recorded boot supports these measurements; the user also
completed the requested page-load test. The verified kernel SHA256 is
`784ce0cdf5fe05532c121acd1847612daa968bb52b3fd3ab52f26405d64e6e1d`.

Local artifacts are in `build/wr128/spi-stream/`: startup logs, extracted card
logs, sparse fixtures, candidate kernels, `comparison.json`, and full source
patches for the whole-protocol and hybrid experiments. Those transports are
not included in the retained driver. `final-kernel.elf` is the tested larger-batch
kernel; `baseline/kernel.elf` retains the previous kernel for comparison.
The physical logs and phase comparison are in `hardware-success/`.

## HSDMA transmit pipeline

The local S1C33E07 manual assigns SPI transmit requests to HSDMA2 (II.1.5).
Unlike the previous IDMA channel 0x24, which sends another dummy word after
RX completion, HSDMA2 can fill TXD at shift start (V.2.5). This removes
IDMA's four-word descriptor load/writeback per word (II.2.4.1) and overlaps
transmit feeding with the current word on the wire.

`SD_DMA_TX=HSDMA` is now the hardware-tested default. It uses HSDMA2 only for
aligned 32-bit payloads; byte transfers retain IDMA. `SD_DMA_TX=IDMA` selects
the previous word path for comparison. The GPIO clock hold, 255-sector
read-ahead, application, and archive layout are retained. Completion waits
for RX, not the earlier TX terminal count. Timeout recovery stops further
TX DMA and lets RX drain the shifting and queued words before counting the
received bytes. A one-unit transfer leaves both TX engines disabled.

The emulator now separates TX-empty from RX-full requests and implements
the one-word transmit buffer, inter-character wait, and pending HSDMA triggers.
The existing timing allowance is retained. The matched local startup run
uses captured exFAT metadata, 253,952 bytes of archive indexes, and the same
boot files; it starts the real file-loader with its inherited stack supplied
by a local emulator harness. It does not emulate the preceding FLASH stages.

| Kernel | Open to keyboard | File/map | File DMA wait |
| --- | ---: | ---: | ---: |
| Installed `b556aa29` binary | 3.989099 s | 3.880008 s | 2.817136 s |
| Current source, IDMA TX control | 3.987747 s | 3.878636 s | 2.813445 s |
| HSDMA2 word TX candidate | 3.309662 s | 3.205248 s | 2.119200 s |

These are **emulator measurements**, not hardware results. The candidate
saves 0.678085 s (17.0%) against its IDMA control. All three file phases issue
30 read commands for 7,396 sectors, with 3,786,752 DMA32 bytes and no read
errors, timeouts, overflow or fallback. The old binary's file phase agrees
with its earlier model result within 1 microsecond; the UI now reads five
more sectors because of the boot-file fixture state.

The model's raw payload floor is 2.019601 s at 15 MHz. SPI_WAIT adds a minimum
four MCLKs between words, and DMA completion/CPU polling add further overhead.
The physical test below checks how closely the pipeline meets that model.
Sensitivity runs with `dma_extra=0`, `30` (the calibrated default), and `60`
all completed without errors. The candidate saved 0.196534, 0.678085, and
1.170955 seconds respectively against the matching IDMA control. This is
sensitivity to one model assumption, not a confidence interval for hardware.
The existing `zimlog.on` marker enables `zimboot.log` and the early `dma.txt`
checkpoint, which now identifies the configured word TX engine.

Artifacts, logs, comparison kernels, and the install manifest are under
`build/wr128/hsdma-tx/`. The focused model tests and all 122 C33 driver cases
pass, including byte order, CRC boundaries, one/two-word transfers, and
mid-block TX stoppage with queued data.

The physical startup/page test passed on 2026-09-08. The recorded boot used
the installed candidate kernel SHA256
`9fd8b22ddc4d2f1746c6550a08bbc0a45c28c330f1d589df0f8c4354e5e17198`
and the same application as the previous hardware test. The card's kernel
and application hashes were checked when collecting the logs.

| Interval | Previous hardware, IDMA | Hardware, HSDMA2 | HSDMA2 model |
| --- | ---: | ---: | ---: |
| Open to keyboard | 4.071086 s | 3.511686 s | 3.309662 s |
| File/map | 3.977882 s | 3.423920 s | 3.205248 s |
| File DMA wait | 2.763685 s | 2.182731 s | 2.119200 s |

Startup fell by 0.559400 s (13.74%), and file DMA wait by 0.580954 s (21.02%).
Every phase's read-command, sector, and DMA-byte counts match the previous
hardware run. All 3,786,752 file-phase payload bytes used word DMA; the log
reports no read errors, timeouts, DMA errors, bypass or disabled/fallback
state. The early checkpoint records `word_tx=HSDMA2` and the physical loader's
`spi_interrupt=00000014`.

The model underestimated total startup by 0.202024 s and file DMA wait by
0.063531 s (3.0% of its prediction). It predicted a 0.678085 s startup saving
against its IDMA control; the measured saving was 0.559400 s. The hardware UI
phase reads 27 sectors versus six in the model fixture, so the DMA-wait
comparison is the closer test of the transport model. These results are one
recorded boot per hardware configuration, plus the user's completed page test.

The capture and comparison are in
`build/wr128/hsdma-tx/hardware-20260909T031723Z/`. The card was cleanly ejected
after capture with the tested kernel and benchmark logging still installed.

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
