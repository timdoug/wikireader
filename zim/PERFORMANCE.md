# Reader performance

What the reader does to be fast, what it costs on the device, and how far the
emulator can be trusted to predict that.

Startup profiling and its card logs are enabled only when the boot volume
contains `zimlog.on`; `zimboot.log` records startup phases and `zimpage.log`
one record per article retrieval.

## On the device

Full English Wikipedia, February 2026 (124 GB) on a 128 GB card, 32 MB
reader:

| | Time |
| --- | ---: |
| Archive open to keyboard | 3511.7 ms |
| Cat, cold retrieval | 2621.1 ms |
| Tokyo, cold retrieval | 3416.3 ms |
| Either article, cached | 55.1 ms |

Retrieval ends before rendering and deferred images. Startup excludes the
FLASH stages and loading the kernel; kernel timer initialization to reader
entry is a further 1398.0 ms.

Simple English and Wikivoyage, June 2026, on the same device:

| | Time |
| --- | ---: |
| Reader initialization | 584.4 ms |
| Cat, tap to first paint | 873.1 ms |
| Tokyo, tap to first paint | 708.5 ms |
| Cached Cat | 65.8 ms |
| Paris, first image | 895.8 ms |
| Paris, complete first paint | 3612.9 ms |

These are single samples, not averages, and not a battery measurement. The
Simple English row predates dropping the bank placement, which the model puts
at about 10 ms on Cat.

## What the time goes on

**Startup is the allocation map.** Opening the full English archive reads
3,786,752 bytes of exFAT allocation metadata in 30 commands and 7,396
sectors, about 3.0 s of the 3.5. The kernel reads it in batches of up to 255
sectors through a shared 127.5 KiB buffer and never scans archive contents.

**Tokyo is glyph I/O.** It reads 576 KB for one article in 825 calls
averaging 1.36 sectors, against Cat's 52 calls of 10.06 sectors. Those two
points fit 55 us per read call plus 677 us per sector — about 756 KB/s
sustained — so batching the calls is worth only about 44 ms of its 807 ms of
I/O. The bytes are the cost, and they are CJK glyphs. This is the largest
identified target and it has not been attacked.

**Cached retrieval is copying.** A revisit reads no sectors and spends its
time on cache snapshot and restore.

## What the reader does about it

- Startup reads only the metadata it needs. Four small fonts preload their
  first 256 records; three large fallback fonts read just their headers and
  fetch glyphs into 2048-slot caches. Header-only loading saves 42 KiB of
  reads and resident allocation. Font fast-seek maps and sector-sized glyph
  fills avoid repeated FAT-chain walks and small card commands.
- Article loading keeps a decoded-cluster cache, four finished articles
  within a 2.5 MiB budget, 16 KiB compressed-input reads, and converts HTML
  straight out of the cached decoder output. Batch copies and compact entropy
  tables reduce SDRAM row changes.
- The Zstandard sequence loop and table builder run in A0 RAM. HTML,
  wrapping, Huffman, WebP and dithering use mutually exclusive IVRAM
  overlays. The WebP overlay occupies 5,220 of its 5,632 bytes; preserve the
  linker bounds and long-call flags when changing these paths.
- Lossy WebP reconstruction produces scaled luma and alpha for the one-bit
  display, skips unused chroma work, uses C33 fixed-point multiplies, and
  accelerates transforms, rescaling and Atkinson dithering. Images stay
  incremental so touch is handled between decoder checkpoints.
- Grifo retains SDRAM retiming, DMA descriptors in DSTRAM, and bounded DMA
  completion polling. Aligned payloads use 32-bit SPI and DMA; commands,
  tokens, CRCs and unaligned transfers stay byte-wide. `SD_DMA_BITS=8`
  selects the previous width and `SDRAM_TIMING=STOCK` the original memory
  timings, for comparisons.
- A bulk-copy helper handles article cache snapshots and restores, the
  article and link-table move, decoded-blob copies and code overlays. It is
  called explicitly on those paths, so ordinary small libc copies pay no
  dispatcher overhead. Word-aligned copies of at least 256 bytes use
  eight-word batches from A0 RAM, including backward overlapping moves;
  disjoint copies of at least 4 KiB out of SDRAM use HSDMA0, into IVRAM or
  within SDRAM. DMA owns the bus in at most 16 KiB chunks and does not
  overlap CPU work or SD transfers.

## The emulator against the device

Measured 2026-09-13 with the same binaries, the same archive and the same
measurement on both sides — `retrieve_us` out of `zimpage.log`:

| Article, cold | Device | Emulator | Ratio |
| --- | ---: | ---: | ---: |
| Cat, retrieval | 2621.1 ms | 2692.3 ms | 1.027 |
| ...its card reads | 356.9 ms | 351.8 ms | 0.986 |
| Tokyo, retrieval | 3416.3 ms | 3386.8 ms | 0.991 |

Cat's work is byte-identical on both: 523 sectors in 51-52 read calls, 25 DMA
copies, 257,024 CPU and 125,004 DMA copy bytes. Tokyo was the second article
of the device session and the first of the emulated one, so only its total is
comparable.

So the model is worth about 3% on a real article load. Use it to identify
expensive work and to compare candidates, and confirm improvements on the
device. It is less trustworthy on synthetic memory loops: `ramspeed` reads
0.78-0.87 on memcpy and 1.09-1.11 on memset, and the instruction-fetch
against data-access overlap behind that is measured but unmodelled. A
2.6-second article load is full of bulk copying and still lands within 3%, so
that gap does not reach anything real here. See the emulator's
[calibration notes](../emulator/README.md#calibration).

## Hardware findings the code depends on

Four things here are the way they are because of something a device did, and
each would look like an unnecessary complication to anyone reading the code
alone.

**An SPI enable cycle costs the card a bit.** The first word-DMA kernels
powered off after the splash while loading the app. A probe read the same
512-byte MBR through every transfer mode and compared payloads, guards, DMA
remaining counts and the card's CRC: byte PIO, byte DMA, word PIO and word
DMA at both 15 MHz and 3.75 MHz all returned a payload shifted one bit left
whenever SPI had been disabled and re-enabled — including an
unchanged-width enable cycle. The reference CRC was `dfa5`; successive enable
cycles shifted it to `bf4b`, `7e97`, `fd2f`. The engines were faithfully
copying an already-shifted stream.

The production helper therefore waits for SPI idle, saves the interrupt and
GPIO state, holds clock pin P67 at CPOL as a GPIO output, clears `SPI_INT`,
disables SPI, sets the width, re-enables, lets the divider settle, and
restores the pin mux and saved state. With that hold, all six payloads and
CRCs match, while a deliberately old transition still shifts. The waveform
was never measured; the emulator reproduces the observed response-bit advance
without claiming which edge causes it.

**HSDMA0's completion cause is indeterminate after reset.** The first
integrated memory-copy build fell back to CPU copying for every one of 63
eligible transfers. Device snapshots showed `FDMA=0x17` with no channel
enabled, no trigger queued and no transfer failed: the app's guard was
reading an uninitialized cause as another owner's pending completion. The
manual says so explicitly (II.1.10 at II-1-46; the FDMA register table at
III-2-42 marks every cause bit's initial value as X). The emulator's
deterministic zero reset concealed it.

The fix acknowledges only HSDMA0's bit, and only with every HSDMA channel
disabled, no pending HSDMA0 trigger, software trigger selected, HSDMA0 IRQ
disabled, write-one-to-clear selected and all its configuration registers
zero. A configured channel's completion stays pending and other channels'
causes are preserved. With it, the device performs the model's exact 25 DMA
chunks for 125,004 bytes on a first Cat retrieval.

**`f_lseek` is alignment-sensitive.** Unrelated kernel code growth moved its
26-byte scan loop across three 16-byte blocks, exceeding the two
instruction-queue slots and adding almost a second. C33 GCC treats that seek
branch as cold and does not align its loop. Check the target disassembly and
the timing after changing it.

**Keep the scan buffer out of the stack.** The current C33 compiler can
schedule a comparison before a large stack-frame adjustment, whose expansion
changes the condition flags before the comparison's branch. With the 16 KiB
buffer on the stack this silently bypassed read-ahead on the device while
passing every host test. The buffer lives in BSS.

## The banks that were not there

The reader used to place buffers that are used together in different SDRAM
banks, on the premise that the controller keeps one open row per bank. It
does not. Reading two addresses 4 MB apart — another bank by the geometry
table — costs the device 140.10 cycles a pass, and two a kilobyte apart,
the same bank, 141.75. Two data addresses evict one another however far apart
they are.

All of it was dead weight. The runtime probe that timed pairs of reads
looking for a stride running at the floor never found one, so it always fell
through to the controller register's word — which is why this file used to
record an unresolved 8 MB stride from the device against 4 MB from the model.
The probe cost 9.8 ms inside the first article load. The copy path's
requirement that source and destination be in different banks never fired
either: every memory DMA in a page load is the IVRAM path, which was already
exempt. Removing all of it left the rendered Cat framebuffer byte-identical.

One measurement is not explained by this and is worth knowing before anyone
tries again. A 2026-09-09 device benchmark of 512 KiB copies found the
layout mattered a great deal to an unbatched libc copy (57.6 ms close
together against 41.0 ms 4 MB apart) and to DMA (37.8 against 20.6), while
barely touching the eight-word A0 batch (34.9 against 33.2). The probe above
measures two read streams, not a copy, so the two do not contradict each
other so much as fail to cover the same case. The model charges no distance
penalty at all, which is right for the case that was measured and untested
for the case that was not.

## Validation

```sh
make -C host-tools/zim-reader check
make -C emulator test-sd test-dma test-sd-dma-driver test-zim-copy
```

The host suite covers archive and cluster caching, HTML conversion, article
links, WebP luma and alpha output, font-cache bounds, and large seek-map
construction with a simulated watchdog, batched metadata reads, FAT
boundaries, pending writes, I/O errors and cyclic chains.
`fastseek-watchdog-test exfat-prefix.bin` opens archives and verifies
fragment-boundary seeks through production FatFs using a captured exFAT
metadata prefix.

The emulator targets run the production C33 driver and copy helper as target
code: 51 driver cases across the byte and word builds covering payloads,
guards, CRC boundaries, recovery, GPIO state and invalid SPI control
accesses, and 708 copy cases covering alignments, tails, overlap, DMA chunk
boundaries, IVRAM, guards, register restoration, busy-channel fallback,
reset-cause initialization and lost-trigger timeout recovery. Neither needs
an attached card or archive.

Full-FLASH emulator runs check startup and the Cat, Tokyo and Paris screens
on 16 MB and 32 MB board configurations.

For new investigations use the emulator's `-F`, `-X` and `-Y`
[profiling options](../emulator/README.md#profiling-and-timing) with symbol
addresses from the app's matching map file. Complete reports and raw
measurements from the first four optimization rounds remain in Git at
`7aa4ee84`, for example `git show 7aa4ee84:zim/PERFORMANCE-ROUND4.md`.
