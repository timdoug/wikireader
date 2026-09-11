# Reader performance

The production reader includes the optimizations validated through 2026-09-09.
Startup profiling and its card logs are enabled only when the boot volume
contains `zimlog.on`; the one-shot hardware probe has been retired.

## Earlier boot progress (2026-09-09, hardware validated)

The interval before "Opening ZIM archive..." mostly precedes the reader's
entry point. In the existing hardware logs, app entry to archive opening
is only about 85 ms. The earlier path loads `kernel.elf`, mounts the card,
writes the optional `dma.txt` checkpoint, and runs `init.app` to load
`zim.app`. The ELF loader zeroes each app's shared LCD reservation before
loading its code, leaving a blank screen during that work.

The kernel now displays "Starting WikiReader..." before filesystem setup,
and redraws it when the ELF loader clears the
shared framebuffer. Filesystem initialization is idempotent. Closing an
app's files still flushes and discards its handles, but retains the mounted
filesystems instead of power-cycling and mounting the same card again.
The ELF loader also keeps a bounded 512-byte seek map while loading an app,
avoiding repeated FAT-chain walks between section headers and payloads.
More than 63 file extents falls back to ordinary seeks.

The startup message now uses the same proportional `text.bmf` font and
vertical position as "Opening ZIM archive...". The build renders that fixed
line into a 512-byte bitmap embedded in the kernel, so showing it still
requires no font-file reads or mounted filesystem. The kernel grows by
256 bytes; emulator captures confirm matching text and an unchanged keyboard.

The new `kernel_to_app_us` field in `zimboot.log` measures from kernel timer
initialization to app entry, without additional startup writes. It excludes
FLASH execution and loading `kernel.elf`; on a later app restart it reports
cumulative uptime.

Validation: normal boot, cold and cached Cat retrievals have zero read/DMA
errors and the same Cat framebuffer as the previous build. A fixture with
`zim.app` split into 682 extents exercises seek-map overflow and boots to
the identical keyboard via ordinary seeks. All three captured startup
message frames match. Local evidence is in `build/wr128/earlyboot/`, including
`result.json`, the phase traces, and screenshots.

Hardware confirmation, captured at 2026-09-10 01:37 UTC: both installed
firmware hashes match the tested build. One new boot and one page retrieval
were appended after installation, and the user reports the earlier startup
display is much better. The measured intervals are:

| Hardware interval | Time |
| --- | ---: |
| Kernel timer initialization to reader entry | 1398.026 ms |
| Reader entry to archive opening | 85.597 ms |
| Archive opening to keyboard | 3375.998 ms |
| Kernel timer initialization to keyboard | 4859.621 ms |

The early checkpoint now records seven DMA-read blocks instead of fourteen,
consistent with removing the duplicate initial mount. Archive opening is
essentially unchanged from the previous two-boot mean of 3378.427 ms. The
old firmware did not record the early interval, so its before/after speedup
remains a model estimate. These times exclude FLASH/menu and loading the kernel.

The page retrieval took 721.921 ms, with eight DMA copies moving 36,852 bytes.
Retrieval timing excludes rendering and deferred images. All startup and
page read/DMA error counters are zero, with no startup timeout or fallback.
The memory-DMA reset flag was acknowledged once, as in the previous build.
Raw logs and `comparison.json` are saved under
`build/wr128/earlyboot/hardware-20260910T013720Z/`. The card was cleanly
ejected after capture; no archive data was read or written.

## Boot model correction (2026-09-09, hardware phase test pending)

The original early-boot fixture predicted 1840.081 ms from the kernel timer
to reader entry, 442.055 ms longer than hardware. Its first `dma.txt` write
cost 647.388 ms, falling to 25.538 ms on the same image's next boot. Unknown
FAT32 FSInfo allocation hints and an absent diagnostic file caused artificial
FAT allocation searches. That 0.64-second estimate was not card programming
latency. The fixture builders now supply valid allocation hints; the model
comparison also retains existing boot files, logs, directory order and
history from the card.

With the exact installed firmware and the captured logical boot-file state,
the corrected prediction is 1315.130 ms versus 1398.026 ms measured: 5.93%
fast instead of 31.62% slow. Kernel-to-keyboard is 4706.437 ms modeled versus
4859.621 ms measured, 3.15% fast. The snapshot contains 14,080,204 bytes of
boot files and no physical archive data. It is not a raw filesystem capture:
physical FAT fragmentation, short aliases and deleted directory slots remain
approximate. Part of the remaining discrepancy may be different I/O work.

The before/after model comparison now uses the same captured logical card
state for both firmware versions:

| Interval, modeled | Before early-boot changes | Hardware-validated early-boot build | Saved |
| --- | ---: | ---: | ---: |
| Kernel timer to reader entry | 1816.856 ms | 1315.130 ms | 501.726 ms |
| Kernel timer to archive opening | 1942.156 ms | 1440.542 ms | 501.614 ms (25.83%) |
| Kernel timer to keyboard | 5210.983 ms | 4706.437 ms | 504.547 ms (9.68%) |

These support an estimate of about half a second saved, in addition to the
progress display remaining visible. Anchoring the modeled early-stage saving
to the new hardware timing and the previous measured app-to-keyboard mean
estimates 5.36 s before versus the measured 4.86 s now (about 9%). The old
early interval was never measured on hardware, so this is not a physical A/B
result. Evidence and formulas are in `build/wr128/model-card/comparison.json`.

The SD model now separates initialization readiness (`sd_init_latency`),
inter-block token delay (`sd_read_gap`), and programming busy
(`sd_write_latency`). All default to zero pending phase-level measurements
on this card; existing read-command, CPU and DMA calibration is unchanged.
This avoids assigning a filesystem mismatch to a hardware delay.

A matching diagnostic kernel/app pair adds bounded RAM snapshots for mount,
`dma.txt`, and each ELF load. `KERNELBOOT` lines in `zimboot.log` report their
elapsed time, sector counts, read time and DMA wait/errors. Formatting and
writing happen after the keyboard appears. The getter is new syscall 120;
install both kernel and app. Its captured-file model predicts 98.100 ms for
mount, 62.928 ms for the checkpoint, 25.688 ms for `init.app` loading and
1088.786 ms for `zim.app` loading. These are predictions for the next test,
not measurements from the already validated firmware.

Validation: SD timing, FAT fixture and focused CPU/peripheral/DMA checks
pass. The diagnostic build boots with both minimal and captured-file fixtures,
and cold/cached Cat retrievals finish with zero I/O/DMA errors and the same
framebuffer. The full emulator `check` target cannot complete in this checkout:
its decoder reference files and stock GUI card image are absent. Local build,
check and model logs are in `build/wr128/model-card/`; hardware phase calibration
is pending the next boot.

The diagnostic pair was installed and its small-file hashes verified at
2026-09-10 02:20 UTC, with the previous firmware and logs backed up under
`build/wr128/model-card/card-backup-20260910T022031Z/`. The card was cleanly
ejected. `installed.json` records the hashes and pending hardware test; the
installation read or wrote no archive bytes.

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

## Memory-copy integration, 2026-09-09 candidate

The [physical memory benchmark](../emulator/tools/mem_dma_bench/README.md)
guides a bulk-copy helper used by article cache snapshots/restores, the
article/link-table move, decoded-blob copies, and code overlays. It is called
explicitly on those paths; ordinary small libc copies do not pay dispatcher
overhead. Word-aligned copies of at least 256 bytes use eight-word batches
from A0 RAM, including backward overlapping moves. Disjoint copies of at
least 4 KiB use HSDMA0 when both ranges fit in separate measured SDRAM banks,
or when copying SDRAM into IVRAM. DMA owns the bus in at most 16-KiB chunks;
it is synchronous and does not overlap CPU work or SD transfers.

The helper checks all DMA channels are idle, preserves the controller and
interrupt settings, and falls back to CPU copying when the channel is busy.
A missing trigger times out after 10 ms if the CPU can run, restores the
controller, repeats the disjoint copy with the CPU, and disables subsequent
DMA attempts. The watchdog covers a controller stall that prevents CPU
execution. These are the same hardware modes that passed the one-shot
benchmark. The first integrated physical run fell back to CPU copying;
the reset-cause fix and subsequent DMA validation are recorded below.

Overlays are padded to word boundaries and copied through that helper.
Libc also now uses forward copying whenever buffers are disjoint; previously
a destination above its source forced a backward byte loop when alignments
differed. Actual overlaps retain backward copying. The libc Makefile tracks
the shared implementation included by memcpy and memmove.

Startup scans the same fresh allocation metadata as before. Its bounded
run-checking loop now runs from the kernel's A0 region, checks four links per
iteration, and returns to geometry/watchdog bookkeeping every 4,096 entries
(16 KiB of cached data), rather than every 128 entries. FAT boundaries,
invalid links, cycles, table sizing and watchdog checks remain enforced.
The kernel's A0 section occupies 668 of 1,024 reserved bytes; the app's A0
code and scratch end at 0x1ea0, leaving 288 bytes before suspend scratch.

`zimlog.on` enables the existing `zimboot.log` and the new `zimpage.log`.
Each page record identifies the build and article, retrieval time, SD reads,
bulk CPU/DMA copy bytes and DMA errors. Retrieval stops before display
rendering and deferred image decoding; log formatting and writes occur
after a render call, outside the measured interval. Reopening a page through
Search exercises both saving the current article and restoring its cache.

Validation uses the actual C33 helper through the Grifo loader: 704 cases
cover alignments, tails, overlap, DMA chunk boundaries, IVRAM, guards,
register restoration and busy-channel fallback. A separate lost-trigger
test checks timeout/CPU recovery. `make -C emulator test-zim-copy` runs both;
build the C33 kernel and libc first. The host reader checks cover FAT maps,
fonts, 168 decoder pairs, 157 cached blob reads, links and truncation.

Model fixtures use the captured card metadata and read requested archive
sectors directly from the existing project files through the emulated
SD/SPI stack. They do not copy or scan the full archives. Baseline and
candidate use matching scripted input on full English Wikipedia, February
2026. Loader-to-app boot is included in execution; earlier FLASH stages are
omitted. The saved model and hardware evidence still shows memory DMA timing
depends on the path, so end-to-end model improvements require hardware
confirmation. Artifacts and the install manifest are in
`build/wr128/load-opt/`.

Final model comparison (one scripted cold load and repeat per page):

| Interval | Baseline | Candidate |
| --- | ---: | ---: |
| Archive open to keyboard | 3309.738 ms | 3231.018 ms |
| File/allocation map | 3205.288 ms | 3125.909 ms |
| Cold Cat, retrieval to render entry | 2291.816 ms | 2295.146 ms |
| Cached Cat, retrieval to render entry | 120.317 ms | 90.410 ms |
| Cold Tokyo, retrieval to render entry | 3567.352 ms | 3580.167 ms |
| Cached Tokyo, retrieval to render entry | 149.390 ms | 109.430 ms |

Startup saves 78.720 ms (2.38%); cached revisits save 24.86% and 26.75%.
Cold loads show no meaningful improvement: they take 0.15% and 0.36% longer
in this model. This change primarily helps cached pages and modestly trims
startup; it does not remove the dominant allocation-map I/O or cold-page
decoder/layout work. The candidate also enables per-page I/O profiling,
which the old baseline lacks. The page records' `retrieve_us` cover the
retrieval function itself; the table additionally includes the intervening
UI work before render entry, and excludes deferred image completion.

Cat and Tokyo produce byte-identical final framebuffers. The final Cat
workflow copies 497,576 bytes in 52 memory-DMA calls; Tokyo copies 537,012
bytes in 52 calls. Both report zero memory-DMA errors. Startup retains 30
file-phase read commands, 7,396 sectors and no storage errors or timeouts.
The original firmware is backed up by the installer. The first physical
comparison follows below.

Installed on WRBOOT and cleanly ejected at 2026-09-09 04:56 UTC. Verified
kernel SHA256: `a617b69850a98b69470dbf5365592841dc5662d12b46016f9efec4ed7661e820`;
app SHA256: `ac5510c8f7455e1e585e6a19087324a6f9e837637443e446784622e610524686`.
The kernel grows by 200 bytes and the app by 2,172 bytes. The original files
and logs are in `build/wr128/load-opt/card-backup-20260909T045642Z/`.

### First integrated hardware run

The logs collected at 2026-09-10 00:18 UTC contain one new boot and seven
successful retrievals. Both installed firmware hashes match the modeled
candidate above. Normal startup remains configured. Evidence is saved in
`build/wr128/load-opt/hardware-20260910T001838Z/`, including raw logs,
small firmware copies, parsed page CSV and a comparison JSON. Collection
read no archive data.

| Hardware interval | Previous firmware, mean of two boots | Candidate, one boot |
| --- | ---: | ---: |
| Archive open to keyboard | 3511.712 ms | 3377.219 ms |
| File/allocation map | 3423.914 ms | 3288.286 ms |
| File-phase SD read time | 3015.044 ms | 3015.065 ms |
| File/map time outside SD reads | 408.871 ms | 273.221 ms |

Startup saves 134.493 ms (3.83%), versus the model's predicted 78.720 ms.
The allocation-map phase accounts for the gain: card read time stays the
same, while processing outside reads falls by 135.650 ms. The file phase
still issues 30 commands for 7,396 sectors; SD DMA wait is 2182.630 ms,
within 0.108 ms of the previous mean. All startup phases report zero read
errors, DMA errors and timeouts, with SD DMA remaining enabled.

| Hardware retrieval, before rendering/deferred images | Time |
| --- | ---: |
| First Cat | 2848.216 ms |
| First Tokyo, after Cat | 3358.422 ms |
| Five cached Cat/Tokyo revisits, median | 76.869 ms |
| Cached revisit range | 76.850-76.905 ms |

The encoded indices identify the five revisits as Cat, Tokyo, Cat, Tokyo,
Cat from History. Each reads zero sectors and uses CPU batches for
1,025,536 bytes of cache snapshot/restore work. All seven records have
successful retrieval results and zero read or memory-copy errors.

**Memory DMA was not exercised in any logged retrieval:** all seven report
zero DMA calls and bytes, while CPU batches account for 6,441,856 bytes.
The model's first Cat retrieval used 25 DMA calls for 125,004 bytes; its
IVRAM overlay copies are eligible independently of heap bank placement.
The current log does not capture rejection reasons, DMA register state,
or failures before the retrieval interval. A rejected idle/pending-channel
guard is a candidate explanation, but these logs cannot establish it.
The next diagnostic needs to record that guard's state and the cumulative
failure latch. Do not treat the error-free page run as validation of the
integrated DMA path, or clear pending controller state without identifying
its owner.

There is no old-firmware hardware page timing baseline, so the predicted
25-27% cached-page improvement is not yet a measured hardware improvement.
The model reopened the same page through Search in separate Cat and Tokyo
runs; this physical run alternated pages through History, changing snapshot
work and I/O. Raw model retrieval times are 2266.773/61.987 ms for cold/cached
Cat and 3547.969/77.181 ms for Tokyo. These exclude the intervening UI work
included in the earlier model table; the differing workflows also prevent
a direct page-speed comparison.

### DMA selection diagnostics

The diagnostic app installed at 2026-09-10 00:38 UTC keeps the copy policy
and DMA guard, and appends `ZIMCOPY` and `ZIMCOPY_STATE` lines after each
`ZIMPAGE` record. Counters are cumulative across the app's lifetime, frozen
at retrieval completion; this exposes failures before the first page's
timed interval. Formatting and writes remain outside that interval.

`large` counts calls of at least 4 KiB. `unaligned`, `identical`, `overlap`,
`source` and `layout` explain ineligible copies; `eligible` counts qualifying
copies and `attempts` counts calls to the chunk helper. `busy`, `trigger`
and `irq` count rejected active channels, pending HSDMA0 triggers and pending
HSDMA0 completion causes. Multiple rejection counters can increment for the
same attempt. `latched` counts skips after an earlier failure; `failed` is
the persistent failure latch. `bank` is the measured bank stride in bytes.

Snapshots preserve the first rejection and first failed transfer, including
source/destination, byte count, active-channel bitmask, HSDMA0 flags, trigger
selectors, transfer registers, clock gate and interrupt enables. Addresses
and register values are hexadecimal; attempt and byte counts are decimal.
`select` packs HTGR1 in its low byte and HTGR2 in its high byte. The snapshots
only read rejected controller state; they do not clear or claim it.

In the local model, injecting either a pending trigger or a pending completion
cause before archive startup reproduces the physical first Cat copy counts:
381,984 CPU-batched bytes, zero DMA calls and zero DMA errors. Both runs
reject 24 eligible copies. The clean-state control uses 25 DMA chunks for
125,004 bytes. The first eligible copy in all runs loads the 4,480-byte
Huffman overlay into IVRAM, independent of heap-bank placement. The working
standalone benchmark clears HSDMA0 flags before each transfer; the app's
guard deliberately rejects pending flags. This explains how the two programs
can differ, but the injected states are hypotheses, not hardware observations.

All three model runs load and reopen Cat and produce the same final screen
as the previous candidate. The C33 helper now passes 707 cases, including
active-channel, pending-trigger and pending-IRQ rejection, preserved flags,
resumption after the test clears its own flags, and retained first snapshots.
The lost-trigger test also verifies the recorded failure state and latch.
The A0 batch's 112 machine-code bytes and all internal-RAM bounds are unchanged.

The diagnostic app is 348,440 bytes (1,668 bytes larger), SHA256
`16a364921266c95f1dfdd1476f8d3c7560b7018fbf00480fc9240bd9d2b354bf`.
The kernel is unchanged. Artifacts, original boot-file backups and the
installation manifest are in `build/wr128/load-opt/dma-diagnostic/`.
The card was cleanly ejected. The following physical run identifies the
reason for the CPU fallback.

### HSDMA0 reset-cause initialization

The diagnostic hardware logs collected at 2026-09-10 00:41 UTC confirm that
**HSDMA0's completion cause blocked all 63 eligible copy attempts**. Every
snapshot reports `enabled=0 tf0=0 fdma=17 edma=0 select=9900 count0=0
control0=0 source0=0 dest0=0 adv0=0` (register values are hexadecimal).
No channel was enabled, no HSDMA0 trigger was queued, and no transfer failed.
HSDMA0's completion bit was set despite its reset-like configuration. The
first attempted copy was the 4,480-byte Huffman overlay into IVRAM.

This matches an explicit initialization requirement in the *S1C33E07 Technical
Manual* (see [ABI.md](../host-tools/toolchain-c33/gcc/ABI.md) for where to
obtain it), II.1.10 (II-1-46): after reset, FHDMx is
indeterminate and software must clear it. The FDMA register table at
III-2-42 also marks every cause bit's initial value as X. The app mistook
that uninitialized cause for another transfer's pending completion. The
standalone benchmark already cleared it; the emulator's deterministic zero
reset choice concealed the missing initialization. No silicon defect is
needed to explain these observations.

The fix acknowledges only HSDMA0's bit, and only with every HSDMA channel
disabled, no pending HSDMA0 trigger, software trigger selected, HSDMA0 IRQ
disabled, write-one-to-clear flag mode selected, and all its standard and
advanced configuration registers zero. A configured channel's completion
remains pending. Other channels' cause flags are preserved. The log adds
`reset_irq` and a first `kind=reset` snapshot taken before acknowledgement.

The C33 suite passes 709 cases, including reset-cause initialization, refusal
to consume an enabled IRQ owner's cause, preserved other-channel causes,
and the existing timeout recovery. The full Cat model seeded with the
observed FDMA bits clears the reset cause once, performs 25 DMA chunks on
the first retrieval and 22 on the cached revisit, and renders the same final
screen. Clean-state and pending-trigger controls also pass; the latter still
uses CPU copying. These are functional checks, not new hardware timings.

Raw hardware evidence is in
`build/wr128/load-opt/dma-diagnostic/hardware-20260910T004156Z/`.
This run also reported an 8-MiB bank stride, while the model reports 4 MiB
and earlier physical spacing tests favored 4 MiB. That separate probe/model
discrepancy remains open; it did not cause the IVRAM-copy rejection and this
fix does not change bank placement.

The next app, its model results and installation manifest are in
`build/wr128/load-opt/dma-reset/`. Physical confirmation should check that
DMA calls are nonzero, errors remain zero, and any captured reset cause was
acknowledged once. A boot where FHDM0 starts clear does not need an
acknowledgement.

Installed and cleanly ejected at 2026-09-10 00:53 UTC. The app is 348,668
bytes, SHA256 `19fba8b68da360d80bf2d021e1d7d1b97fbc4c61009f9be176ae9c2762654d28`;
the kernel is unchanged. The initialization fix adds 228 bytes to the
diagnostic app. Its physical confirmation follows.

### Hardware confirmation of memory DMA

The logs collected at 2026-09-10 00:59 UTC verify the installed app and
kernel hashes, two new boots and four successful retrievals. On each boot,
the first eligible copy captures `FDMA=0x17`, acknowledges HSDMA0's reset
cause once (`reset_irq=1`) and proceeds with DMA. Every captured cumulative
attempt completed through DMA; busy/trigger/IRQ rejections, failure latch
and DMA errors remain zero. There are no read errors or startup SD timeouts.

The first Cat retrieval uses 25 DMA chunks for 125,004 bytes on both boots,
exactly matching the model's copy counts. The complete second sequence is
Cat, Tokyo, then Cat through History; its last snapshot reports 107 completed
DMA calls, including copies between the timed retrievals.

| Hardware retrieval | Previous diagnostic, CPU fallback | Reset fix, DMA enabled |
| --- | ---: | ---: |
| Cached Cat after Tokyo, through History | 76.871 ms | 64.845 ms |
| First Tokyo after Cat | 3358.952 ms | 3354.402 ms |

The matching cached revisit saves 12.026 ms (15.64%). It copies 554,696 bytes
through 35 DMA chunks and 470,880 bytes through CPU batches, with zero SD
reads. This is one matching revisit per build and measures retrieval before
rendering/deferred images. It demonstrates a hardware benefit; it is not a
general percentage for all pages or a comparison with the original libc-only
firmware.

Cold Cat takes 2866.222 and 2579.796 ms on the two boots of this same app;
that variation exceeds the expected DMA-copy savings. Tokyo changes by only
4.550 ms and reads one extra sector. These samples do not establish a large
cold-page improvement. Startup remains stable at 3378.417 and 3378.437 ms
from archive open to keyboard.

The 8-MiB bank-stride result repeats. Resolving its difference from the
model and earlier bank-spacing benchmark remains a separate optimization
question; memory DMA now works with the current hardware allocation policy.
Saved logs, firmware identities and parsed comparison:
`build/wr128/load-opt/dma-reset/hardware-20260910T005935Z/`.

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
| Installed `9adc6d9d` binary | 3.989099 s | 3.880008 s | 2.817136 s |
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

The optimization commits are `05e89f09`, `c80f0b19`, `3ef83afa`, and
`7aa4ee84`. Complete reports, raw measurements, and the retired benchmark
harness remain in Git at `7aa4ee84`, for example:

```sh
git show 7aa4ee84:zim/PERFORMANCE-ROUND4.md
```

For new investigations, use the emulator's maintained `-F`, `-X`, and `-Y`
[profiling options](../emulator/README.md#profiling-and-timing) with the
symbol addresses from the app's matching map file.
