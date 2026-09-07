# Startup and image performance, 2026-09-07

This records the first round, committed as `207044cd`. See
[round two](PERFORMANCE-ROUND2.md) for the subsequent scaler and drawing-buffer
experiments and their separate device A/B pair. [Round three](PERFORMANCE-ROUND3.md)
adds WebP transforms, brightness-only reconstruction and startup probes.
[Round four](PERFORMANCE-ROUND4.md) uses those probes to reduce fallback-font
startup reads.

Compared with `5472952f`, using GCC 16.2, the current kernel, stock ZIM
archives and the calibrated emulator's 32 MB board (`WREMU_BOARD_REV=7`).
The hardware A/B pair measures 9.5% less reader initialization time, 25.0%
less first-photo setup/decode/dither time, and 8.1% less full Paris page-load
time. See the paired hardware results below; there is one sample per build.
The model still underestimates article phases on the device; the following
emulator numbers compare builds, not predict device time.

| Work | Before | After | Reduction |
| --- | ---: | ---: | ---: |
| Reader initialization, after the launcher | 1970 ms | 1863 ms | 5.4% |
| First Wikivoyage Paris photo: WebP setup, decode and dither | 1447 ms | 1021 ms | 29.4% |
| Simple English Cat: text page preparation | 719 ms | 715 ms | within layout noise |
| Simple English Tokyo: text page preparation | 696 ms | 695 ms | within layout noise |

The photo takes 1025 ms on the emulator's 16 MB board. Both final photo
framebuffers, the startup screen, and the Cat/Tokyo screens match the
baseline byte for byte.

## Device results

The physical-card run is saved in
[`bench-device-2026-09-07-images.txt`](bench-device-2026-09-07-images.txt).
The card's normal and benchmark app SHA-256 hashes match the staged files;
the benchmark identifies the September 7 16:03:28 GCC 16.2 build. It ran on
the 32 MB board with the existing kernel and stock Simple English and
Wikivoyage June 2026 archives.

| Text article, tap to first paint | Previous device run | Updated device run |
| --- | ---: | ---: |
| Cat | 871.6 ms | 864.1 ms |
| Tokyo | 705.0 ms | 704.9 ms |
| Japanese Bobtail | 1087.6 ms | 1089.0 ms |
| Cat, reopened from history | 60.7 ms | 60.3 ms |

The uncached baselines are from the final, 15:14:53 build in
[`bench-device-2026-09-07-after.txt`](bench-device-2026-09-07-after.txt);
the cached baseline is from its preceding 14:56:16 build. Each is a single
sample, and all differences are below 1%, consistent with unchanged text
performance. Article hashes, byte counts and decompression-call counts
match. Internal stack high-water usage remains 292 of 1024 bytes.

The first Paris photo is 226 by 167 pixels from 11096 compressed bytes:

| Image phase | Updated emulator benchmark | Updated device benchmark |
| --- | ---: | ---: |
| Setup | 0.4 ms | 0.4 ms |
| Decode | 938.8 ms | 1067.8 ms |
| Dither | 64.4 ms | 88.3 ms |
| Sum | 1003.6 ms | 1156.5 ms |

Its bitmap hash is `6653db08` on both, and the Paris article stream hash is
`316a1e7e` on both. The device takes 15.2% longer for these image phases than
the same benchmark build in the emulator. The normal-build profile above
also includes time between phase measurements, so its 1021 ms is a
different measurement. Paris's complete device article load is 3703.5 ms,
including archive extraction, text processing, painting and image-log I/O.

This initial deployment had no instrumented hardware image baseline or
startup timer. It verified the resulting image hash and text timings, but
could not measure those gains. The subsequent A/B pair supplies the missing
comparison below.

## Paired hardware verification

The first deployment lacked an instrumented baseline and a startup timer.
It therefore could not verify the startup/image gains on hardware. A proper
A/B pair is now installed, with instructions and exact binaries in
`build/performance-2026-09-07/ab/README.md`.

- **A BEFORE**, second launcher row on the left: original `5472952f`
  performance implementation, including its original `memset`, with the
  same measurement hooks as B.
- **B AFTER**, second row in the middle: the optimized implementation.
- Both omit calibration micro-benchmarks, start with Simple English, and
  time initialization automatically through font loading. This excludes
  factory boot, kernel boot and loading the application ELF.
- Logs append to separate `ab-before.txt` and `ab-after.txt` files. Power
  cycle between variants, open Cat, switch to Wikivoyage and open Paris,
  then wait for its first photograph. Repeat the pair for more samples.

These exact benchmark binaries pass paired emulator tests: initialization
1987.3 to 1880.2 ms; Paris image setup/decode/dither 1408.2 to 1005.9 ms;
Cat 803.0 to 802.7 ms; complete Paris page 3716.4 to 3311.6 ms (including
benchmark I/O). Both apps were selected through their labelled launcher
entries on writable images. Their saved files match serial output, and
Cat/Paris framebuffers match byte for byte. Source differences and validation
logs are preserved beside the binaries.

The returned hardware logs are
[`bench-device-2026-09-07-ab-before.txt`](bench-device-2026-09-07-ab-before.txt)
and [`bench-device-2026-09-07-ab-after.txt`](bench-device-2026-09-07-ab-after.txt).
Both installed app hashes still match the staged, emulator-tested pair.
The archive, SDRAM settings and timer scale match between runs.

| Hardware measurement | A BEFORE | B AFTER | Time reduction |
| --- | ---: | ---: | ---: |
| Reader initialization | 828.7 ms | 749.7 ms | 79.0 ms / 9.5% |
| Cat, complete page | 869.5 ms | 871.5 ms | effectively unchanged (+0.2%) |
| Paris first photo, setup/decode/dither | 1476.3 ms | 1107.6 ms | 368.7 ms / 25.0% |
| Paris, complete page including benchmark I/O | 4183.4 ms | 3844.5 ms | 338.9 ms / 8.1% |

Image setup remains 0.4 ms, decoding falls from 1304.7 to 1018.9 ms
(21.9% less time), and dithering from 171.2 to 88.3 ms (48.4% less).
The 226 by 167 bitmap hash is `6653db08` in both runs. Cat and Paris article
hashes remain `3c7a9642` and `316a1e7e`, with identical byte counts and
decompression-call counts. Internal stack high-water usage is 292 of 1024
bytes in both runs.

These are single A/B samples, not repeated-run averages. They substantiate
the startup and image gains on this device/card; the whole-page result
includes benchmark logging overhead in both variants. Device initialization
is substantially shorter than the emulator's, whose boot filesystem uses
smaller FAT clusters. The separate decoder/dither timers provide the clearest
image comparison and do not include log writing or archive extraction.

## Changes

- WebP's coefficient decoder and large-value helper run in an IVRAM
  overlay. Dithering has its own overlay, including its three error rows.
  The linker checks code plus data against the 5632-byte window. The two
  overlays occupy 2946 and 3552 bytes and reuse existing internal RAM.
- The boolean coder's 256-byte log table uses the A0 scratch area between
  text wrapping and image completion. Its bounded range uses a direct
  lookup instead of the general-purpose log loop.
- The WebP bit reader now inlines its small copies. The old Makefile's
  `-fbuiltin` preceded the shared rules' `-fno-builtin` and had no effect.
  Target-specific flags enable it for `vp8_dec.o` and `zim_image.o`.
- Incremental decoding exposes a growing prefix of the resident compressed
  blob through `WebPIUpdate`, avoiding another input buffer and copying.
  The 128-byte touch-service checkpoints remain.
- C33 `memset` fills eight words per loop. A C unroll made startup slower:
  extension instructions grew the loop beyond the CPU's two 16-byte
  instruction-queue slots. The final aligned, 20-byte loop uses
  postincrement stores. Alignment, short fills, trailing bytes and nonzero
  fill values retain their behavior.

These gains are in the application; the supplied binaries use the existing
kernel. Other programs rebuilt against mini-libc also get the fill change.

## Remaining costs

Font setup still takes about one second of initialization in the emulator.
It eagerly opens seven fonts and builds FAT seek maps, including three
3.5 MB CJK fonts. The test image's FAT32 boot volume has 512-byte clusters;
the physical card has 4096-byte clusters, so its seek-map startup cost can
differ substantially. Deferring font setup would move latency to the first
non-Latin article. Batching FAT-map reads is a useful next experiment.

Tokyo spends heavily on font I/O while wrapping. SPI function time includes
waiting for card data tokens, so a large SPI profile alone does not mean
DMA failed. Cat's largest remaining CPU cost is Zstandard sequence decoding,
much of it producing preceding articles in the same stock ZIM cluster.
This round preserves the archive format.

## Reproducing the measurements

Build mini-libc from the root (its own Makefile uses `CROSS`, not
`TOOLCHAIN_BIN`), then the reader:

```sh
make TOOLCHAIN_BIN="$PWD/host-tools/toolchain-c33/work/install/bin" mini-libc
make -C zim -B -j8 TOOLCHAIN_BIN="$PWD/host-tools/toolchain-c33/work/install/bin" zim.app
```

Install that app and the matching kernel into a scratch image made with
`zim/make-card-image`, verify the copied bytes, and detach it. Keep the
matching `zim.map`. From the repository root:

```sh
python3 zim/perf-emulator.py /tmp/simple.dmg /tmp/startup --phase startup
python3 zim/perf-emulator.py /tmp/simple.dmg /tmp/cat --word CAT
python3 zim/perf-emulator.py /tmp/simple.dmg /tmp/tokyo --word TOKYO
python3 zim/perf-emulator.py /tmp/voy.dmg /tmp/paris --phase image --word PARIS
```

Output directories must be new; each gets a log, address profile and
framebuffer. This assumes the normal two-entry launcher with ZIM first
and a normal, non-benchmark app. Use `--map saved.map` for a saved binary
and `--board 8` for the 16 MB board.

Startup runs from `wikilib_run` to the first search-renderer call.
`grifo_main` is invalid because init.app uses the same address;
`wikilib_init` is inlined. Article preparation runs from `retrieve_article`
to the first article-renderer call, excluding paint and lazy images. Image
timing runs from decoder creation to destruction, including setup and
dithering but excluding ZIM lookup/extraction.

## Validation and device files

- Emulator `make check` passes. Its manual-timing SDRAM test had failed
  because two new calibration parameters were not reset with the older
  ones; the test now resets both. The timing model is unchanged.
- Host `make -C host-tools/zim-reader check` passes, including 157 cached
  blob reads compared with independent decompression.
- A dedicated C33 image app checks five patterned images: lossy/lossless,
  opaque/alpha, upscaling/downscaling and native size. Bitmap hashes match
  the original host implementation; all half-length compressed inputs are
  rejected. Hashes: `31045f00`, `7aa254ae`, `21d851d4`, `bafb3be7`, `fb7242c6`.
- `host-tools/toolchain-c33/tests/runtime/memset-check.c` exercises 5936
  cases with eight alignments, seven values, lengths 0-96 and boundaries
  through 4097 bytes, checking return pointers and guards. It exits with
  status zero in the emulator.

Prepared files are in `build/performance-2026-09-07/`: normal `zim.app`,
`zimbench.app`, its icon, the original `zim-before.app`, and validation
artifacts. The normal build is also left at `zim/zim.app`.

The benchmark variant now logs `image WIDTHxHEIGHT ... setup ... decode
... dither ... bitmap fnv ...` to `bench.txt`. Image phase times exclude
archive extraction and result logging. Writing that additional line can
increase the benchmark app's article paint time; compare the image fields
directly. Article blob accounting stops at its blob milestone so image
reads during painting cannot inflate it and underflow the `other` field.
