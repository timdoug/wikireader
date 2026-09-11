# Doom on WikiReader

`doom.app` runs the original software-rendered game on Grifo and the Epson
C33 PE CPU. It uses [PureDOOM](https://github.com/Daivuk/PureDOOM), with an
indexed 320x200 renderer converted to a 240x180 monochrome image and a
28-pixel touch control strip. The original game's 4:3 display aspect is kept.

The port runs on a physical WikiReader and in the full-system emulator.
The first hardware benchmark measured **14.73 fps** at the E1M1 opening, versus
16.26 fps in the matching instrumented emulator run; see
[physical measurements](PERFORMANCE.md) for the full comparison.
Audio and networking are disabled. Low detail is the default; drawing,
LCD conversion and division loops plus dither and lighting tables occupy about 4.8 KiB
of the application's available A0 RAM. A 4 KiB flat-texture cache uses the
disabled LCD window buffer, separate from the visible framebuffer. It uses this repository's Grifo kernel,
including its `file_fastseek` API; the current kernel and reader application
do not need changes.

## Build

From the repository root, with the [modern toolchain](../host-tools/toolchain-c33/README.md)
and Grifo libraries already built:

```sh
make -C doom -j4
make -C doom test
make -C doom test-c33
```

The Makefile defaults to `host-tools/toolchain-c33/work/install/bin`.
Override `TOOLCHAIN_BIN` for another installation. Outputs are `doom/doom.app`
and `doom/doom.ico`. The build is offline: engine source is vendored and pinned
in [vendor/README.md](vendor/README.md).

## SD card

Copy `doom.app` and `doom.ico` to the root of a working WikiReader card. Create
a `doom` directory and put your IWAD there under its standard filename:
`doom1.wad` (shareware), `doom.wad`, `doomu.wad`, `doom2.wad`, `tnt.wad`, or
`plutonia.wad`. The shareware episode was used for validation; the other games
have not been exercised. WAD game data is separate from the engine source.

Add this entry to the card's existing `init.ini`:

```text
doom.ico : doom.app
```

Keep the existing reader entry to retain a choice at boot. With a single
launcher entry, Grifo starts that application automatically; quitting Doom
will then start it again. To start directly in the first level:

```text
doom.ico : doom.app -warp 1 1 -skill 2
```

Configuration and saves live under `/doom`. In Save Game, choose a slot and
confirm it again to use the name `WIKIREADER`. Existing save names are kept.
Use Quit Game to save settings and return to the Grifo launcher.

Startup console output is quiet by default to avoid waiting for the slow
serial port. Add `-wrverbose` for the full startup diagnostics. Fatal errors
always print, including in quiet mode.

## Persistent performance tracing

For a repeatable device/emulator comparison, use this launcher entry:

```text
doom.ico : doom.app -wrbench
```

It starts E1M1 at skill 2, forces low detail and the normal view size, and
uses `/doom/bench.cfg` separately from normal settings. Input is ignored
while the bottom strip says **BENCH**: after the level transition there is
a five-second warmup and a ten-second stationary measurement. The regular
control strip returns afterward and you can play. Use the same shareware
IWAD and application build on the device and emulator.

Results are appended to **`doomperf.log` in the card root**, including across
restarts. Boot stages, warmup and the benchmark are buffered in RAM and written
and closed together after measurement. Tracing then turns off for normal play,
avoiding periodic card-write stalls. Allow the benchmark to finish before
powering off; an interrupted benchmark has no persisted boot record.
Use `-wrbench -wrtrace` to keep tracing afterward, with a closed batch about
every five seconds and a final partial batch when you quit.
The logger stops at 1 MiB, preserving earlier runs; archive/remove the log to
start fresh. A write failure displays `LOG FAILED` and releases the controls.

The log contains a source build fingerprint, clock/SDRAM register values,
boot checkpoints, kernel loader timing records, SD read/DMA counters and
errors, frame counts, elapsed time, engine and LCD time, BSP/wall, floor and
masked-sprite time, frame-time min/max and histogram, simulation tic counts,
view settings and player state. Phase values are **sums**, in microseconds;
BSP/planes/masked are subsets of engine time. Engine's remaining time includes
simulation, HUD, menus and transitions; LCD time includes the small frame
probe/state-snapshot overhead. Histogram bins end at 33, 50, 67, 100, 150, 250,
500 ms and infinity. Normal windows report mixed scene counts explicitly.

The benchmark writes nothing to SD or serial during its measured window.
Normal windows include logging stalls in elapsed time and the frame histogram;
`FLUSH` separately reports each preceding write/close duration. SD I/O counters
exclude the log writes. All timing state stays outside the renderer's fast RAM.
The 32-bit timer is extended across frames for long sessions. Application boot
time starts at app entry: it excludes FLASH/kernel loading and time spent
choosing an icon. `kernel_epoch_raw` is a wrapping kernel timer reading, not a
power-button-to-frame measurement. `KERNELBOOT` records cover loader work after
that kernel timer starts, also excluding the earlier FLASH/kernel load.

For tracing ordinary title/menu startup and play without the automatic
benchmark, launch `doom.app -wrtrace`, or create an empty `doomlog.on` in the
card root and launch `doom.app`. Ordinary tracing persists boot records after
the first frame and continues about every five seconds. Remove the marker and
omit `-wrtrace` to disable it. With `-wrbench`, only an explicit `-wrtrace`
keeps tracing after measurement; the marker does not.

Build a matching emulator reference and summarize a returned hardware log:

```sh
python3 doom/trace-benchmark.py build/doom/doom1.wad
python3 doom/trace-report.py /Volumes/WRBOOT/doomperf.log \
  --compare build/doom/trace-EXAMPLE/doomperf.log
```

The reference run retains its actual FAT log, app/map, binary hashes, modeled
FPS, external frame probe and JSON report. The comparison requires a completed,
stationary benchmark with matching source fingerprint, settings and player
state; the install manifest supplies application/IWAD hashes to verify the
exact inputs. The first physical run and its model differences are recorded in
[PERFORMANCE.md](PERFORMANCE.md).

## Controls

The game view acts as a three-by-three directional pad. Hold the upper third
to move forward, the lower third to move backward, the left third to turn
left, and the right third to turn right. Corners combine movement and turning.
The center is neutral. Lift your finger to stop. Buttons can be held while
touching the screen.

| Control | Playing | In menus |
| --- | --- | --- |
| Random | Fire | Confirm / Yes |
| Search | Use / open doors | Down |
| History | Open menu | Back / Cancel |
| Touch top / bottom | Forward / backward | Up / down |
| Touch left / right | Turn | Adjust setting |
| Touch center | Neutral | Confirm |

Bottom strip: **MENU**, **MAP**, **<S** (strafe left), **S>** (strafe right),
**GUN** (next owned weapon), **RUN** (toggle running). Power retains the normal
device power-off behavior. Brief presses are held for at least one simulation
step so an entire tap arriving during a slow render is not discarded.

## Emulator

These commands create new regular image files and never access a mounted card:

```sh
mkdir -p build/doom
python3 doom/make-flash.py build/doom/flash.rom
python3 doom/make-card.py build/doom/doom.img /path/to/doom1.wad
emulator/wremu -g -e build/doom/flash.rom -c build/doom/doom.img
```

Press **P** to power on; mouse clicks are touches, and **1/2/3** are
Random/Search/History. Add `--args '-warp 1 1 -skill 2'` when making the card
to bypass the title/menu. Image builders refuse to overwrite existing files.

The emulator FLASH fixture replaces only the file-loader payload with a
kernel-only loader that fits its 7,424-byte slot. The current full loader
overlaps the next diagnostic slot when linked against the modern libraries.
This fixture is for the emulator; physical devices keep their factory FLASH.

## Checks

`make test` runs the actual clock, file, input, and display adapters against
the Grifo public ABI with address/undefined-behavior sanitizers. It covers
timer wrap, file seeks/EOF/overwrite, taps queued during a frame, simultaneous
movement and fire, menu mappings, monochrome polarity and framebuffer bounds.
The renderer regression checks fixed-point division against 64-bit reference
arithmetic and low-detail span/column boundaries, including translated and
invisible sprites. Both suites run under address/undefined-behavior sanitizers.
The LCD tests compare every output pixel against the original scalar converter,
including random palettes, every sampling/dither phase and cached palettes.
File tests cover lazy seek-map construction, fragmented-map resizing, allocation
and I/O failures, and keeping the map alive until close. Sprite tests verify
eight-byte reads, signed offsets and subsequent full-image reads.
Texture-cache checks cover reused WAD allocation addresses, lump changes,
engine reset, small surfaces that retain an existing cache entry, and holes
in a plane's visible area. Wall-patch tests compare metadata-only lookup with
full-image lookup and overlapping-patch composites. Random span checks exercise
fractional wrap, lighting-table alignment, offset buffers and odd span lengths.
Logger tests cover deferred benchmark writes, long transitions, opt-in continuous
tracing, early errors and card-write failures.

`make test-c33` compares 227,672 native arithmetic results with host reference
calculations. It runs the actual C33 multiply instruction and calls the
reciprocal helper in A0, covering signs, overflow-bit truncation, accumulator
clobbers, all divisors from 1 through 65,536, power-of-two boundaries and
random inputs. It also checks general unsigned division/remainder and the
compiler's signed and unsigned `/` and `%` entry points through the A0 bridge.

`tests/replay.py` compares 1,500 frames of the built-in DEMO1 at one simulation
tic per frame, including indexed pixels, palettes and player state, under
AddressSanitizer. Pass an IWAD and `--reference /path/to/saved/source`, which
must contain `engine.c` and `vendor/PureDOOM.h`. The pre-optimization reference
used here includes the same WAD filename allocation fix as the current build;
its rendering code is otherwise unchanged. The latest cache pass was also
compared directly against the preceding released build.

To run automated full-device tests with a shareware IWAD:

```sh
python3 doom/smoke.py /path/to/doom1.wad
```

This creates separate disposable cards for firing/movement, saving/loading,
and starting a new game from the title screen. It drives the real touch/button
interrupts, checks engine function-entry counts, and retains screenshots,
serial logs and an app/WAD hash report under `build/doom/smoke-*`.

For target execution, `doom_frame_ready` in `doom.map` marks each displayed
frame and can anchor `wremu -Z` inputs or `-X` measurements. Full-FLASH boot is
required for representative LCD setup and memory timing; direct kernel ELF
boot skips that setup. Emulator timing is an estimate, not a hardware FPS
measurement.

The scripted shareware E1M1 run on 2026-09-10 displayed 146 frames, averaging
5.8 fps between its first and last frame (including the transition and automap
interaction). Its first frame appeared at 23.9 seconds of modeled guest time.
The save/load run wrote and reloaded a `WIKIREADER` save on the FAT32 card.
Neither run reported an alignment fault or watchdog timeout.

## Performance

The current loading and lighting-cache pass improves the controlled E1M1
benchmark from **16.26 to 17.00 modeled fps (4.5%)**. An ordinary stationary
run with input polling enabled improves from **16.0 to 16.7 fps** over guest
seconds 30-40. The controlled benchmark uses a five-second warmup and ten-second
measurement, with input locked; these are separate workloads.

These are emulator results against the committed hardware-tested build. The
physical baseline is still **14.73 fps**; this new build needs a device run.
See [PERFORMANCE.md](PERFORMANCE.md) for phase timings and retained artifacts.
Renderer resolution, texture sampling and LCD output are unchanged, and the
1,500-frame replay matches the preceding build exactly.

The latest FPS gain comes from copying aligned wall/floor lighting tables as
words, four per iteration, instead of bytes. The byte fallback handles unaligned
sources. An experimental wider floor-write loop added complexity without a
useful measured gain and was discarded.

Earlier optimizations reduced LCD conversion from roughly 79 ms to 7 ms per
frame, avoided repeated texture/pointer reads in the low-detail draw loops,
combined duplicate pixels into one halfword write, and moved exact fixed-point
division into internal RAM. Earlier passes added C33's native signed multiply, specialized
the hot unsigned texture-scale divisions, moved floor span generation/mapping
into A0, and cached the current wall/floor light tables there. The complete
1,500-frame replay comparison matches the preceding renderer.

Wall setup and general division/remainder also run in A0.
The original libgcc entry points reach the exact divider through a short
SDRAM bridge, retaining signed division and remainder behavior. Floor and
ceiling textures can use the 4 KiB LCD window cache: a new texture is admitted
when its plane covers at least 2,048 texels, and small planes leave the current
entry intact. Keys use WAD lump identities rather than reusable zone addresses.
The WAD allocation remains separate for zone tagging. `memory.lds` reserves
the cache and rejects conflicting Grifo overlays; Doom disables the LCD window
before the engine can fill it. No clock or SDRAM timing changes are involved.

To reproduce the fixed-scene benchmark:

```sh
python3 doom/benchmark.py build/doom/doom1.wad
python3 doom/benchmark.py build/doom/doom1.wad --scene demo
```

The demo option measures recorded movement and combat in the shareware IWAD's
built-in DEMO1, and verifies that shots occur in the measurement window.
Faster startup shifts which demo tics fall in seconds 30-40, so use the stationary or controlled
benchmark to isolate FPS gains across loading changes.
Each run creates a fresh card and retains the app, map, profile, screenshot, log and JSON
result under `build/doom/benchmark-*`. Use `--app /path/to/doom.app` and
`--map /path/to/doom.map` together to measure a saved build. The figure is
scene-specific; busy rooms and different view settings will differ.

The prepared emulator card with the current rendering and startup improvements
is `build/doom/next-after/card.img`:

```sh
emulator/wremu -g -e build/doom/next-after/flash.rom -c build/doom/next-after/card.img
```

It starts with fresh configuration and saves. On a physical card, replace
only `doom.app` with the rebuilt file to keep existing settings and saves.

## Startup

Cold boot to the first title frame measures **4.39 seconds**, down from
**6.27 seconds** in the preceding committed build and **22.90 seconds** before
the original startup optimizations. Booting directly into E1M1 with
`-warp 1 1 -skill 2` takes **6.87 seconds**, down from **7.56 seconds**, including
level precaching and its first frame. These are emulator timings using the same
shareware WAD and FAT32 fixture with 512-byte clusters. The instrumented hardware build
measured **4.38 seconds from app entry to its first E1M1 frame**, versus
5.78 seconds for the same scope in its emulator reference. This excludes
FLASH/kernel loading and the launcher wait; see [PERFORMANCE.md](PERFORMANCE.md).

Large read-only files now build a cluster map on their first seek, so
backward WAD reads do not repeatedly traverse the FAT allocation chain.
The map is optional and retained until close; fragmented files can request
a larger map, with bounded memory use and a fallback to ordinary seeking.
Sprite initialization reads just dimensions and offsets, leaving image data
to the existing level precache or on-demand loader. Sound-disabled builds
also skip construction of the unused volume mixing table.

Wall texture initialization now reads only patch widths and column directories.
A temporary cache shares these small directories across textures, then releases
them. Actual patch pixels are loaded by level precaching or when first used.

To reproduce cold-start timing and retain a startup profile:

```sh
python3 doom/boot-benchmark.py build/doom/doom1.wad
python3 doom/boot-benchmark.py build/doom/doom1.wad --args '-warp 1 1 -skill 2'
```

Each run creates a fresh card and stops after the first displayed frame.
Results, startup-stage times, profiles, screenshots, and copies of the app
and map are stored under `build/doom/boot-*`. Use `--app` and `--map`
together to measure a saved build. No persistent startup cache is required.
