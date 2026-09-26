# Game Boy on WikiReader

A Game Boy (DMG) emulator for the WikiReader, built on
[Peanut-GB](https://github.com/deltabeard/Peanut-GB) at the revision in
`revision` with `patches/peanut.patch` applied. The Game Boy's instructions,
timing, rendering and memory paths are rewritten for the C33; Peanut
supplies the cartridge controllers, the I/O registers and everything rare,
and serves as the reference every rewritten path is checked against. There
is no sound.

## Build

```sh
make gameboy            # from the repository root; or make -C gameboy
make -C gameboy test    # host builds: build/host, host-check, host-lockstep
```

The first build clones Peanut-GB into `work/`.

## Card layout

Copy `gameboy.app`, `gameboy.ico` and the game to the card's root, with a
launcher line naming the game:

```text
gameboy.ico : gameboy.app tetris.gb
```

Names are 8.3. A game with battery RAM keeps it in the same name with
`.sav`, written whenever the game's saved data changes (checked every five
seconds) and on Quit. `threshold` after the game's name draws the two grays
as white and black instead of dithering them; `serial` copies what the game
sends on the link port to the serial console, for test ROMs that report
there.

## Display and controls

The 160x144 picture is drawn 1:1 in the panel's top right corner. Its four
shades become white, a quarter of the pixels black, half of them black
(a checkerboard), and black; `threshold` makes them white, white, black,
black.

The front buttons are Start, B and A (Search, History, Random), in the order
a Game Boy has them. The D-pad is drawn bottom left: a touch there, or
anywhere in the column left of the picture or the strip below it, is the
direction from the cross's centre, eight ways, with a dead zone in the
middle, and a finger that slides off it keeps steering until it lifts.
Select is bottom right and Quit top left. Buttons can be held while
touching.

## How it is fast

- **`gen-hot.py` -> `hot.s`**: the CPU's common instructions in C33
  assembly. The Game Boy's registers stay in C33 registers, the flags are
  kept lazily (Z as the result byte, C in bit 31, H and N in one register),
  dispatch is threaded through a 256-entry table, and a cycle budget in a
  register stops the loop exactly at the next timer, serial or LCD event.
  It runs the event itself through `gb_hot_event()`, registers pushed, and
  only leaves for an interrupt or the end of the frame. Anything it does not
  do -- most I/O, banking writes, HALT, EI, DI, RETI, DAA, the stack in HRAM
  -- it gives back to C, which runs that one instruction through Peanut.
  Code in HRAM, where games keep the loop that waits out OAM DMA, runs in it
  as a region of its own; through HL it also reads HRAM and the I/O
  registers that change only at events (LY, STAT, IF), and stores the
  scroll and window positions and LYC. The heaviest handlers by
  `opcode-weights.txt` go in A0 RAM, the LCD window buffer and the default
  framebuffer; the rest run from SDRAM.
- **Idle loops**: `ldh a,(nn); and a; jr z` (or `jr nz`) on an HRAM flag,
  the way games wait for their VBlank handler, is fast-forwarded once A and
  the flags show it can only go round again: whole passes are charged at
  once, the last one stepped to find the instruction each event falls
  after, and events run in turn until one leaves an interrupt pending. It
  leaves at the loop instruction a step-by-step run would have reached.
- **`memory.h`**: a 16-page memory map for the C paths, and timing that is
  deferred until the next event. Only DIV and TIMA fall behind meanwhile;
  an access that meets them, or a write that could move the next event,
  runs the deferred cycles first. `event_tick()` runs one event touching
  only what changes and returns the next budget; `ticks()` does the rest,
  and Peanut's HALT loop -- about six passes a halted scanline -- with the
  counters and I/O registers held in locals for the whole wait. Both run
  from the window buffer.
- **`render.h`**: each scanline as big-endian 32-bit bit planes, so fine
  scrolling is a funnel shift, and palettes, sprite priority and the dither
  are masks, in one pass from tile fetch to framebuffer on lines without
  sprites. Sprites are bucketed by line when OAM changes.
- **On-chip RAM for state and code**: Peanut's `struct gb_s`, without its
  work RAM and VRAM, is about 540 bytes and lives at the start of the
  kernel's default framebuffer, 6.6 KB of zero-wait IVRAM the panel never
  shows while the emulator runs: the controls are drawn straight into the
  emulator's own buffers. The kernel's loader clears that framebuffer and
  then loads handlers over the rest of it (`.fbcode`, `memory.lds`). Frames run on a
  private stack in the 1 KB of DSTRAM the kernel's SD DMA descriptors leave
  free; the benchmark reports the depth reached, and a canary stops the
  emulator if a frame overflows it.

## Accuracy and checks

The emulator does what Peanut-GB does, including where Peanut departs from
the hardware: sprites behind the background compare shades with BGP colour
0's shade, a line's ten sprites are the ten with the lowest X, and there is
no HALT bug.

- `build/host-lockstep GAME FRAMES [SCRIPT]` steps the fast paths beside an
  unmodified Peanut (`tests/reference.c`), compares the registers after every
  instruction and all of RAM, I/O, counters and banking wherever nothing is
  deferred.
- `build/host-check GAME FRAMES [SCRIPT]` runs Peanut's renderer beside
  `render.h` on every line; `build/host-check -fuzz N` does it on N frames of
  random VRAM, OAM and LCD registers.
- `run.py` boots the real chain (FLASH loader, kernel, init.app) in the
  emulator, runs a scripted benchmark and compares the C33 build's frame
  hashes -- picture, WRAM, VRAM, OAM and I/O every 60 frames -- with
  `build/host`'s. `hot.s` only runs on the C33, so this is what checks it.

## Measurements

`run.py GAME [--script S] [--frames N] [--window W]` times the frames after
W, flat out; `--profile` adds a PC profile for `hotspots.py`, and
`--play CYCLES` boots the game normally and keeps the panel as a PNG.
Emulator (wremu) figures, real time being 59.73 frames a second:

| Workload | Speed |
| --- | ---: |
| Libbet and the Magic Floor, gameplay demo (frames 421-480) | 185% |
| Pokemon Red, intro (frames 421-540) | 176% |
| Tetris, first piece falling (frames 721-840) | 171% |
| Link's Awakening, storm and beach intro (frames 421-540) | 118% |

Tetris's script presses Start at frames 350, 450, 550 and 650; Link's
Awakening needs none. Tetris and Link's Awakening busy-wait for VBlank
instead of halting and run about 5,300 Game Boy instructions a frame, three
times what Pokemon and Libbet do; the idle-loop skip covers their wait.

The benchmark report also counts, per frame, the calls into `hot.s`, the
instructions it gave back and which opcodes they were.

Figures move by a few percent when unrelated code changes size, because
Peanut's step function still runs from SDRAM. Not yet measured on the
device.

Libbet is `pinobatch/libbet` v0.08 (zlib licence); `run.py`'s default script
presses A at frame 300 and Select at 400 to start its demo. The test ROMs
are `retrio/gb-test-roms` and `mattcurrie/dmg-acid2`.
