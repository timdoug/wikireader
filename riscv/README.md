# rv32ima on the WikiReader

An rv32ima interpreter that runs as a Grifo application on the C33, and a
bare-metal RISC-V benchmark image to measure what a guest instruction costs.

The target is the configuration Linux calls nommu M-mode
(`CONFIG_RISCV_M_MODE` with `CONFIG_MMU` off): no Sv32, no page walks, no
supervisor mode. That is what keeps a Linux-capable core small enough to fit
in the chip's internal RAM, and it is the reason the memory map here is the
one mini-rv32ima established — RAM at `0x80000000`, an 8250 UART at
`0x10000000`, CLINT at `0x11000000`, SYSCON at `0x11100000`. A marker device
at `0x11200000` is ours: the guest writes a kernel id to it and the host
timestamps the write, which is how per-kernel costs are attributed without
the guest needing a clock it can trust.

## Build and run

```sh
make                 # riscv.app plus the guest image build/rvbench.bin
python3 run.py       # run the benchmark; about two and a half minutes
make test            # interpreter vs a native build of the same kernels
make ASM=0           # without the assembly hot path, C only
python3 compare.py   # build every placement and run them all, one table
python3 hotspots.py --app build/riscv-code+state.app   # C cycles by line
```

## From the icon panel

```sh
./fetch-linux.sh                 # kernel and device trees, once
make                             # riscv.app, the icons, the guest benchmark
python3 run.py --launcher --gui
```

Boots to Grifo's launcher with two entries: **RV32** runs the interpreter
benchmark, **Tux** boots Linux. Press the window's power control to
start the machine, then tap an icon. The panel is a 3x3 grid of 64-pixel
icons from (8, 4) with 8- and 4-pixel gaps, so icon centres are x = 40,
112, 184 and y = 36, 104, 172 -- which is what `--tap 40,36,CYCLE` aims at
for a scripted run.

The launcher only draws the panel for two entries or more; given one it
chains straight into it, which is why the card carries both.

`../run-launcher.py` is the same boot with every application the tree
builds -- Kiwix, DOOM, NuttX and both of these -- which is the panel the
SD card in `../install-card.sh` produces. Row pitch is 68 pixels, so the
second row's centres are y = 104.

## Linux directly

```sh
python3 run.py --gui --image linux/Image --dtb linux/wr-sh.dtb
```

That opens the panel in a window. Press the power control to start the
machine, the way the hardware does, and wait: about nine minutes of wall
clock for thirty-one seconds of guest time, most of it unpacking the
initramfs. Then the shell prompt appears on the panel and the on-screen
keyboard below it takes mouse clicks as touches.

The panel is a 40x13 terminal in the X11 misc-fixed 6x9 font, with four
rows of 22-pixel keys under it: the same geometry, layout and font as the
NuttX terminal on this machine, so the two behave the same. Shift, Ctrl and
the symbol layer invert their key while they are on, and the three case
buttons are Ctrl-C, Escape and Tab.

To drive it from a script instead, with the output on your terminal:

```sh
python3 shell.py --command 'uname -a' --command 'ls /'
```

`shell.py` boots Linux, waits for the shell prompt and sends each command,
printing what comes back. It waits rather than guessing: the boot is around
nine minutes of wall clock for thirty-one seconds of guest time, and the
instruction count it lands on is not stable between builds.

To watch a boot without driving it, and get the emulator's own statistics
at the end:

```sh
python3 run.py --image linux/Image --dtb linux/wr.dtb \
        --limit 80000000000 --timeout 3500
```

`wr.dtb` boots to a login prompt: the user is `root` with no password
(`etc/shadow` in the image's initramfs has an empty password field).
`wr-sh.dtb` adds `rdinit=/bin/sh` and lands straight in a shell instead,
which is the one that has been driven end to end. With an initramfs the
kernel runs `/init` and ignores `init=`, which is why it has to be
`rdinit=`.

To type at it over the serial line rather than the keyboard:

```sh
python3 run.py --image linux/Image --dtb linux/wr-sh.dtb \
        --type 'ls /\n' --type-at 40000000000 --type-gap 400000
```

Keep `--type-gap` at 400000 or more. The emulated UART holds six bytes and
the application drains it once per batch of guest instructions, so anything
tighter loses the start of a line to FIFO overrun.

`compare.py` takes about fifteen seconds end to end, and verifies from the
map file that each build actually got the placement it asked for --
consecutive makes land inside one filesystem timestamp tick, and make will
otherwise hand every variant the same binary. `run.py` boots Grifo with
the application as `init.app` on a disposable FAT32 card, so the SDRAM
controller is in the state the device runs in and the emulator's memory
model is switched on; a direct ELF boot would model no memory system at all.

`SCALE=n` multiplies the guest workload, which defaults to about 1.3 million
instructions.

## What it costs

Measured in the full-system emulator, whole benchmark, cycles per retired
guest instruction:

| build | cyc/insn | guest speed |
| --- | ---: | ---: |
| all in SDRAM | 403.0 | 148 kIPS |
| interpreter in A0 RAM (`FAST=1`) | 123.3 | 486 kIPS |
| and machine state too (`STATE=1`) | 96.3 | 623 kIPS |
| hand-written hot path (`ASM=1`) | **73.3** | **819 kIPS** |

On silicon the last row is **80.5 cyc/insn, 745 kIPS**.

## The device against the model

All four builds were put on a card and run — RV32, SDR, A0, A0S — one
guest workload with more and more of the interpreter in internal RAM, which
is the axis the disagreement lay along. Their reports are checked in as
`rv*-device.txt`, so **`fit-model.py` needs no hardware**:

```sh
python3 fit-model.py                       # score the model as it stands
python3 fit-model.py --sweep dq_hit=0,1,2  # score a parameter at each value
```

The three extra builds are no longer on the card or in the launcher: they
were there to be measured, they have been, and a developer benchmark does
not want a permanent square of the panel. `compare.py` still builds all
four for the emulator, which is what `fit-model.py` scores against.

Whole benchmark, device over model, before any of this was fixed:

| what runs where | model | device | ratio |
| --- | ---: | ---: | ---: |
| all in SDRAM | 390.5 | 353.2 | 0.905 |
| code in A0 RAM | 123.7 | 119.9 | 0.970 |
| code and machine state in A0 RAM | 96.6 | 101.2 | 1.047 |
| assembly hot path, state and table internal | 73.6 | 80.5 | 1.093 |

and as it stands, after `dq_iram_extra`, `branch_bubble`, `act_overlap` and
the fetch lookahead: **0.979, 0.872, 0.979, 1.010**, RMS log error 0.0786
against 0.0927 when the first of these was measured.

**Every instruction count is identical**, kernel for kernel, in all four —
which is the first thing the runs prove: the hand-written hot path retires
exactly what the C interpreter and the native reference do, on hardware
the host tests cannot reach.

Two errors of opposite sign, which is why the single asm measurement was
so hard to read. The ratio climbs monotonically as code moves into
internal RAM, and within each build it climbs again from the kernels that
are mostly guest memory traffic (crc, sieve) to the ones that are almost
pure interpreter (alu, branch). `div` is the tell: its cycles are mostly
libgcc's divide, which `memory.lds` puts in the LCD window buffer in
*every* build, and it is the one kernel undercharged even in the
all-in-SDRAM column — 1.005 where everything around it is 0.90.

That pointed at internal-RAM execution, and the cause turned out to be a
missing charge rather than a wrong one: `dq_iram_extra`, which prices a
data read issued by code not itself coming over the SDRAM bus, had been
deleted from the emulator's `schedule_read` while its parameter and
documentation stayed. Restored, `code+state` lands at 0.999 and `asm` at
1.030. The all-in-SDRAM row is a separate, still-open error — code
fetched from SDRAM is modelled about 10% too expensive. `emulator/README.md`
has the detail.

The benchmark reports somewhere the device can keep. Each run writes
its table beside the image it measured — `rvbench.bin` gives
`rvbench.txt` — with `file_create`, which replaces the previous run rather
than appending, so the file always describes the run that just finished.
The panel gets the summary line only; it is 40 columns and the table is
wider. Passing `hold` after the image name leaves that summary on screen
until a key is pressed, which is how the card entry in `install-card.sh`
is written: the run takes under two seconds on the device, and without it
the numbers are gone before you can read them. In the emulator the hold
stays off, because the wait would be minutes of modelled time for a number
already in the log.

If the card will not take the file, the application refuses to power off
and waits for a key instead — cutting the rail is what would destroy the
only remaining copy. `run.py` checks the card image after every run and
fails if the report is not on it.

Placement is worth 4.2x and the assembly a further 1.31x. Two things pay
for the placement: SDRAM instruction fetch, and the 27-byte loop residency
window, which a dispatch loop cannot satisfy at any alignment. The state
move is worth a further 27 cycles on its own — that is what the guest
register file costs when every read and write of it is an SDRAM row access
on top of the guest's own fetch.

Internal RAM is the budget everything competes for:

| region | holds | used |
| --- | --- | ---: |
| A0 RAM, `0xc00` | hot path, machine state, the divide helper | 4,352 of 5,056 |
| LCD window, `0x81a00` | the 1024-entry dispatch table, libgcc's divide | 4,852 of 5,632 |

## Why the assembly is faster, which is not instruction count

A taken branch costs about six cycles, so what a guest instruction pays for
in control flow dominates what it pays for in work. The C interpreter's
path has three taken branches — the opcode dispatch, the funct3 dispatch
and the loop back. `rv32_hot.s` has one:

- **The dispatch table is indexed by the opcode and funct3 together.** 1024
  entries, which is why it lives in the window buffer rather than A0 RAM.
  There is no second dispatch for the ALU operations, the load and store
  widths or the branch conditions — each is its own table entry.
- **The fetch is threaded.** Every body ends by fetching and dispatching the
  next instruction itself instead of jumping back to a shared loop head.
  A0 RAM is zero-wait, so the code this duplicates costs nothing to fetch.
- **The operands are extracted once, before the dispatch.** x[rs1] and rd's
  byte offset are in registers when a body starts. Most of what the
  compiler spends its seventy instructions on is re-deriving fields of the
  instruction word that were already in a register.

A first version that kept a shared loop head and a second-level funct3
dispatch measured **slower than the C** it replaced: 101.7 cyc/insn against
92.4, on five taken branches per guest instruction.

The hot path implements the shapes that dominate real code and declines
everything else — system instructions, atomics, misaligned accesses, device
addresses, a fetch outside RAM. `rv32_run` then has the C interpreter
execute exactly that one instruction and calls back in, so every rare case
is still written once, in C. The divides are the exception: they go to a
small C helper called directly from the assembly, because declining costs a
round trip through the whole interpreter and real code divides often enough
for that to show. Doing that took `div` from 486 cyc/insn to 343.

## What the C33 rewards, measured

Each of these was measured alone against the same benchmark. In the C
interpreter:

- **Dispatch on a table, never on a C `switch`.** On the 7-bit opcode gcc
  emits two range checks, a 53-entry table and a compare chain: 22 C33
  instructions per guest instruction. On the dense 5-bit key it still emits
  a range check and needs the low two bits tested separately. A computed
  goto over all 128 opcodes is six instructions and needs neither. Worth
  19 cycles.
- **Give the two commonest opcodes their own bodies.** OP and OP-IMM shared
  one, which costs an `is_reg` test in the operand fetch and again in the
  add/subtract, on every arithmetic instruction. Worth 7 cycles.
- **Test pc alignment where pc changes, not where it is used.** Branch and
  JAL immediates are multiples of two, so both can misalign, but only
  control transfers can. Worth 2 cycles.
- **Keep cold code out of internal RAM.** The MMIO paths were being inlined
  into `.fastcode`; marking them `noinline` freed 380 bytes and cost
  nothing.

And what measured *worse*, which is the more useful half:

- **Carrying pc as an offset into guest RAM** to make the bound test a
  single compare: 19 cycles worse. gcc already folds `ram - RV_RAM_BASE`
  into a loop-invariant base, so the fetch was never paying for it, and the
  conversions on jumps and links cost more than the test saved.
- **Comparing against both ends of guest RAM** instead of one compare
  against a subtraction: 6 cycles worse. One more live constant is one more
  spill.
- **Writing x0 and zeroing it again** instead of branching around the
  writeback: 5 cycles worse.
- **Counting the batch down to zero** instead of up to a limit: 1 cycle
  worse; recovering the retired count costs more than the compare saves.
- **Reconstructing the retired count at exits** instead of incrementing a
  64-bit field per instruction: worse when the base was kept in a 64-bit
  local, better as a 32-bit `published` delta. Two registers is the
  difference.

The pattern in the C is consistent: past the first few structural wins that
core has no registers to spare, and anything that adds a live value loses
more than the instructions it removes. That is exactly the constraint the
assembly does not have, and most of its 1.31x is register allocation the
compiler could not do.

## Correctness

Every kernel prints a checksum and `make test` diffs those against the same
C compiled for the build machine, so a run that reports a plausible speed
but a wrong answer is caught. The sieve is independently checkable: it
returns 2262 at the default scale, which is π(20000).

The host test cannot run C33 assembly, so `run.py` compares the checksums
the guest prints against that same reference on every emulator run. That
is the only thing standing between a fast interpreter and a wrong one, and
the benchmark deliberately exercises what the assembly implements
separately: signed and unsigned divide and remainder, and all three
multiplies.

## Layout

- `rv32.c`, `rv32.h` — the interpreter. Portable C; the C33-specific part is
  three section attributes and where the build puts them.
- `rv32_hot.s` — the hot path. Implements the common opcodes and declines
  the rest to the C interpreter.
- `console.c`, `font6x9.h` — the terminal on the panel: 40x13 characters
  over a four-row keyboard, following the NuttX terminal's geometry, layout
  and font so the two consoles on this machine match.
- `memory.lds` — the window-buffer reservation: the dispatch tables, and
  libgcc's division, which is otherwise the worst code in the program.
- `riscv.c` — the Grifo application: loads an image, runs it in batches,
  reports, and cuts the power rail so a run costs the benchmark and nothing
  more.
- `guest/` — the bare-metal rv32ima benchmark, built with
  `riscv64-unknown-elf-gcc -march=rv32ima_zicsr`.
- `tests/host_run.c` — runs a guest image under the interpreter on the build
  machine; `tests/ref.c` is the native reference for the checksums.
- `compare.py`, `hotspots.py` — the two measurements worth taking: what a
  placement is worth, and which source line is spending the cycles.
- `make-icons.py` — regenerates the two launcher icons. The XPMs are
  checked in, so the build needs no network.

## Credit

Tux is Larry Ewing's (lewing@isc.tamu.edu), made with The GIMP. The
launcher icon is his image downsampled from the Linux kernel's
`drivers/video/logo/logo_linux_mono.pbm`, inverted: the kernel draws it
white on black for a framebuffer console, and this panel puts black ink on
white.
