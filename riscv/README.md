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
python3 hotspots_asm.py                # the assembly path, by instruction
make mix             # what a Linux boot executes, natively, in a second
python3 run.py --args jit              # what translated code would cost
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
| all in SDRAM | 349.8 | 171 kIPS |
| interpreter in A0 RAM (`FAST=1`) | 122.0 | 491 kIPS |
| and machine state too (`STATE=1`) | 99.1 | 605 kIPS |
| hand-written hot path (`ASM=1`) | **75.4** | **796 kIPS** |

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
| A0 RAM, `0xc00` | hot path, machine state, the divide helper, the probe's code buffer | 4,804 of 5,056 |
| LCD window, `0x81a00` | the 1024-entry dispatch table, libgcc's divide | 4,852 of 5,632 |

## What a Linux boot executes

Every number above is against `guest/bench.c`, which is a mix chosen to
exercise the interpreter rather than one anything really runs. `make mix`
runs the same interpreter over `linux/Image` on the build machine, one
instruction at a time, and says what a real guest does instead: thirty-eight
million instructions from reset to the shell prompt, which is nine minutes
under the full-system emulator and seven tenths of a second here.

| | op-imm | load | store | op | branch | jal | jalr | lui | mul | div |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Linux | 29.0% | 20.0% | 18.0% | 14.9% | 9.2% | 3.3% | 2.7% | 1.6% | 0.56% | **0.01%** |
| `rvbench` | 30.9% | 11.1% | 8.0% | 28.5% | 19.1% | 0.02% | 0.00% | 0.00% | 1.83% | 0.59% |

Two differences matter. Linux moves memory: 38% of its instructions are
loads and stores against the benchmark's 19%. And it does not divide — 2,128
divides in the whole boot, one instruction in eighteen thousand, where the
benchmark's `div` kernel is 10% of its cycles. The divide is the most
expensive thing the interpreter does and it is worth nothing here.

**It declines one instruction in 78.** The hot path hands back to the C
interpreter for device addresses (0.82% of all instructions, mostly the UART
the kernel prints through), CSR and system instructions (0.30%), atomics
(0.14%) and misaligned accesses (0.02%). The benchmark declines one in 5,076,
so this cost appears in none of the numbers taken against it. The device
accesses no longer decline -- `rv32_hot.s` calls the helpers itself, and
`rv32.h` puts them in the window buffer -- which leaves the CSRs and the
atomics, and those turn out to be the expensive ones.

Control flow is looser than the benchmark's: 8.16 instructions between
control transfers, and 64% of branches taken against 88%.

The code is small and the hot part of it is tiny:

| covering | blocks | guest instructions |
| --- | ---: | ---: |
| 50% of everything executed | 89 | 3,638 |
| 80% | 585 | 9,577 |
| 90% | 1,235 | 46,748 |
| all of it | 16,574 | 642,574 |

98,713 distinct instructions are executed in the whole boot — 385 KiB of
guest code — and 89 basic blocks account for half of it. The hottest single
block is 1,902 instructions of straight-line ChaCha20, entered 2,367 times
and 11.9% of the boot on its own; the five after it are the kernel's memcpy
word loop, its unrolled copy, its memset, its memcpy byte loop and memcmp,
another 15% between them.

And the guest almost never writes to code it has run: **26 pages in the
entire boot**, one per 1.46 million instructions, 25 distinct pages of the
372 it executes from.

`make mix` also times the boot between one console line and the next, which
needs no marker chosen in advance and catches the phases nobody printed the
start of. Two thirds of the boot -- 25.2 million instructions of 37.9 -- is a
single gap that ends at `Serial: 8250/16550 driver`, and nothing is printed
inside it. It is the initcalls, and the 4 KiB spans the instructions land in
say what they are doing: 17.2% in the kernel's memcpy and memset, 13.6% in the
add-xor-rotate block at `0x800e7000`, 4.7% in the nanosecond arithmetic at
`0x80046000`. **No decompressor appears anywhere**, and the reason is that the
image carries its initramfs as a plain cpio -- 245 `070701` headers in the
clear at file offset `0x1917c4`. `populate_rootfs` is the largest single
thing this boot does and what it costs is the copy, not an unpacking.

## What a translator would have to hold in registers

The templates above say register allocation is worth 5.5x. Whether it is
reachable is a question about the guest, and `make mix` answers it: for every
basic block, which guest registers it touches and which it reads before it
writes. Weighted by the instructions executed in blocks of that size:

| registers | blocks touching at most that many | ...live in at the top of the block |
| ---: | ---: | ---: |
| 4 | 17.8% | 66.4% |
| 6 | 40.1% | 84.9% |
| 8 | 56.2% | 88.8% |
| 10 | 66.5% | 91.5% |
| 16 | 81.9% | 96.1% |

The C33 has about nine registers to spare once the register-file base, the
address adjustment, both ends of guest RAM and a scratch pair are live. Two
thirds of everything executed is in a block whose entire working set fits in
that, and **89% is in a block needing eight or fewer values loaded at entry** —
so the prologue a translated block pays to get started is four to eight loads
from internal RAM, not thirty-two. What does not fit is the long unrolled
crypto, which is where an allocator would have to spill; that is the ordinary
job of a linear scan and not a reason to skip one.

## Where the cycles go

`hotspots_asm.py` attributes the emulator's per-address profile to the
instructions of `rv32_hot.s`, gathering all forty copies of `DISPATCH` into
one row each. Per retired guest instruction, on the Linux boot:

| | cycles | share |
| --- | ---: | ---: |
| `DISPATCH` — fetch, decode, dispatch | 40.1 | 53% |
| the bodies' own work | 14.2 | 19% |
| operand and writeback macros | 15.2 | 20% |
| the C interpreter, for what is still declined | 5.5 | 7% |
| **total** | **75.1** | |

Inside `DISPATCH`, the guest instruction fetch — one `ld.w %r5,[%r1]` from
SDRAM — is **11.1 cycles** and half a row activation. The indirect jump that
ends it is 2. The other 27 cycles are twenty-five instructions that take the
instruction word apart, at about a cycle each, and put it back together as a
table index and two register offsets.

So about two thirds of every guest instruction is decode and dispatch: work
that depends only on the instruction word, and that a translator would do
once instead of every time.

The last row is what a declined instruction costs: a return to C, one
instruction interpreted from SDRAM, and a re-entry. It was 6.99 cycles a
guest instruction, 546 a decline, until the device accesses stopped
declining -- two thirds of them by count, and `rv32_hot.s` now calls the
helpers itself. That took the whole boot from 76.79 cycles a guest
instruction to 75.07, and it says something about the rest: the device
accesses were the *cheap* declines. What is left costs about 890 cycles
apiece, which is what a CSR instruction or an atomic goes through in the C
interpreter, and at 0.44% of the instruction stream that is 3.9 cycles a
guest instruction still on the table.

A profile has to be windowed to the run to say any of this. `pc_profile`
buckets are cumulative and grifo boots from the same A0 RAM the interpreter
is later loaded into, so its card-reading SPI loop and the dispatch macro
share addresses: unwindowed, 2.2 million executions of grifo's loop land on
four instructions of `DISPATCH`, and the hot path comes out a fifth dearer
than the run it was measured in. `run.py --window START,END` passes the
emulator's `-Y`, and both hotspots tools now bracket the run with `rv32_run`
and `power_off`.

The start address has to be one only the application reaches, which
`rv32_hot` is not: grifo runs its own code from the same A0 RAM, so a window
opened at `0xcf4` opens during grifo's boot and takes in the 3.4 MB card read
that loads the kernel. That is worth 11 cycles a guest instruction of SD and
SPI, and it is why this section once reported 81.3 where the run is 76.8.

## What translated code would cost

`jit_probe.s` holds what a translator would emit for the blocks a Linux boot
spends its time in, hand-written and honest about the whole cost — the guest
register file addressed through an `ext` prefix, guest addresses checked
against both ends of RAM the way `rv32_hot.s` checks them. Each template is
position-independent, so `riscv.c` copies it into SDRAM and then into A0 RAM
and times the same bytes in both. This is a question for the hardware:
`emulator/README.md` has SDRAM-resident code about 10% too dear and two
walking streams 20 to 30% too dear, and translated code is both at once. So
the numbers here are the device's, in cycles per guest instruction:

| template | guest | SDRAM | A0 RAM |
| --- | --- | ---: | ---: |
| `alu_reg` — ChaCha20's quarter round, values in registers | 20 a round | **3.50** | 1.28 |
| `alu_mem` — the same, every operand through the register file | 8 a pass | 19.85 | 11.63 |
| `ld_free` — a guest load, bound test hoisted | 5 a pass | 30.01 | 15.73 |
| `ld_check` — the same load, checked on every access | 5 a pass | 40.35 | 20.54 |
| `copy_reg` — the memcpy word loop, pointers in registers | 5 a pass | **5.22** | 5.46 |
| `copy_mem` — the same loop, nothing held, every access checked | 5 a pass | 28.93 | 15.00 |
| `exit_none` — eight blocks, each laid out after the last | 64 a pass | 2.78 | 1.08 |
| `exit_link` — the same eight, chained by patched jumps | 64 a pass | 5.71 | 474 B |
| `exit_hash` — the same eight, chained through a lookup | 64 a pass | 10.80 | 474 B |

Device numbers, from `rvjit-device.txt`, against the interpreter's 80.5 on the
same silicon. The last two are SDRAM only: 474 bytes against the 316 A0 RAM
has left, and on the evidence of `copy_reg` a code cache belongs in SDRAM
anyway. Four things here decide how a translator should be written.

**Register allocation inside a block is worth more than everything else put
together.** The same five guest instructions of the kernel's memcpy are 5.22
cycles each with the three pointers in C33 registers and 28.93 with none:
twelve bytes of code against a hundred, 15.4x against 2.8x. The ALU pair says
the same, 3.50 against 19.85. Hoisting the bound test out of a loop is a
further 34% on a load, 40.35 to 30.01.

**A code cache in internal RAM is not worth building.** `copy_reg` is the
fastest thing here and it is *faster in SDRAM than in A0 RAM* — 5.22 against
5.46 — because twelve bytes fit the 27-byte window a loop has to be inside to
be fetched once rather than every pass, and because a data read issued by code
running from internal RAM is charged for it. Internal RAM is worth 1.7x to the
templates that hold nothing in registers and nothing at all to the ones that
do: it compensates for bad code generation and does not reward good. The
interpreter needs it because a dispatch loop cannot satisfy that window at any
alignment. Translated blocks can.

**A block exit costs more than the block.** `exit_none`, `exit_link` and
`exit_hash` are the same eight blocks of eight instructions each: run straight
through, chained with a patched jump apiece, and chained through the lookup an
indirect guest jump has to use. 2.78, 5.71 and 10.80 cycles a guest
instruction, which is

| exit | cycles |
| --- | ---: |
| successor laid out next, no jump at all | 0 |
| a patched direct jump | 23.4 |
| a lookup, which `jalr` has no alternative to | 64.1 |

against a body of eight instructions that costs 22. A Linux boot leaves a
block every 8.16 instructions and 23% of those exits are `jalr`, so an average
exit is 32.8 cycles and **block exits alone are 4.0 cycles a guest
instruction** -- as much as the body of a well-translated block.

Two things follow for a translator. Lay blocks out along the hot path rather
than as isolated units, because a successor placed immediately after its
predecessor costs nothing at all and one placed anywhere else costs 23.
And cache return addresses: most `jalr` are returns, and a call site that
pushes the translated return address turns a 64-cycle lookup into a 23-cycle
jump on the commonest indirect branch there is.

**The model was wrong in the direction nobody expected.** SDRAM-resident code
is charged about 10% too much for the interpreter, so these templates were
expected to come in under it; they came in over, by 4 to 18%, and the error
tracks SDRAM *data* traffic alongside the code stream rather than the code
itself. `alu_reg` in SDRAM, a pure instruction stream, is exact to the
hundredth. `ld_free` and `ld_check`, which walk a data stream underneath that
code stream, are 14 and 18% dear — while the same loads issued from A0 RAM are
exact. The one internal-RAM row that misses is `alu_mem`, 19% dear, whose data
is the register file in A0 RAM: dense internal loads and stores from internal
code. Both are recorded in `emulator/README.md`.

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
- `jit_probe.s`, `jit_probe.h` — what a translator would emit for the blocks
  a Linux boot spends its time in, copied into SDRAM and into A0 RAM and
  timed in both. `install-jit-probe.sh` puts it on a card and takes it off
  again.
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
  `tests/host_mix.c` runs one there too, stepping it, and reports what the
  guest executed rather than what it cost.
- `compare.py`, `hotspots.py`, `hotspots_asm.py` — what a placement is worth,
  which C source line is spending the cycles, and which instruction of the
  assembly path is.
- `make-icons.py` — regenerates the two launcher icons. The XPMs are
  checked in, so the build needs no network.

## Credit

Tux is Larry Ewing's (lewing@isc.tamu.edu), made with The GIMP. The
launcher icon is his image downsampled from the Linux kernel's
`drivers/video/logo/logo_linux_mono.pbm`, inverted: the kernel draws it
white on black for a framebuffer console, and this panel puts black ink on
white.
