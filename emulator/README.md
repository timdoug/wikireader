# wremu - WikiReader full-system emulator

`wremu` boots the WikiReader's Epson S1C33E07 firmware through the
serial-FLASH boot chain, attaches a FAT32 card image, and presents the
240x208 touch display through SDL2.

## Everything boots the way the hardware does

```sh
emulator/wremu -e flash.rom -c card.img
```

Mask ROM, MBR, file-loader, `kernel.elf`, `init.app` -- the whole chain,
every time. `samo-lib/mbr/make-flash.py` builds a FLASH image for a run.

There is no shortcut, and that is deliberate. A direct ELF boot skips the
loader, so the SDRAM controller, the PLL and the serial line are left in a
state the hardware is never in, and the emulator has to invent plausible
values for them. Four separate bugs have come from that gap: a guest clock
25% slow, a memory model that was not switched on at all, CPU benchmarks
5x off, and a console that could not receive a byte because
`init_rs232_ch0()` had never run. Each looked like a bug in the thing being
developed.

`--bare-elf IMAGE` still runs an ELF with no boot. **It is only for the
toolchain test suites** -- `tests/dejagnu`, `tests/abi` and
`doom/tests/c33_math_test.py` -- which run compiler output rather than
firmware and have no loader to go through. Nothing that runs on a
WikiReader may use it, for development or for measurement.

Build the firmware with the [modern C33 toolchain](../host-tools/toolchain-c33/README.md).
See the [ZIM reader guide](../zim/README.md) for archive and card setup.

## What it runs

The mask-ROM effect, MBR, menu, file-loader, kernel, `init.app`, `wiki.app`,
`zim.app`, `doom.app` and the NuttX port all boot from FLASH and card images.
Shipped GCC 3.3.2 and current GCC 16.2 firmware reach matching UI, search,
article and scrolling framebuffers for the exercised workflows.

`make check` covers the decoder, core ISA, exceptions, interrupts, LCD,
display input, SD, DMA, clocks, ADC, timers, watchdog, SDRAM, GPIO and chip
identification. It completes on a bare checkout; three targets skip and say
so, wanting objdump captures or a local card image.

Headless execution is deterministic and runs at about 80 million target
instructions per host second.

Timing follows the programmed clocks and memory/storage registers, with
additional costs calibrated on hardware. See "Calibration" for the fitted
parameters and what absolute timing predictions are worth.

## Build and test

```sh
cd emulator
make
make check
```

SDL2 is required for the window (`brew install sdl2`). Generated C33 decode
tables are committed, so the cross-compiler is not required for a normal
emulator build.

`make check` runs all focused model tests. `make difftest` runs the generated
native-versus-C33 execution comparison; see
[`difftest/README.md`](difftest/README.md).

`make test-sd-dma-driver` additionally compiles the production SD receive
backend as C33 code and runs both `SD_DMA_BITS=8` and `32`. It covers alignment,
byte order, CRC boundaries, partial-transfer recovery, overflow, profiling,
busy-register access, and GPIO/interrupt restoration. It requires the project
C33 toolchain (or `TOOLCHAIN_BIN=/path/to/bin`) and creates a temporary 512-byte
card image; it does not access attached cards or archives.
Both `SD_DMA_TX=IDMA` and the default `HSDMA` configuration are tested,
including one-word payloads and stopping TX with a word queued behind the
shift register. There are 122 C33 cases across the four configurations.

## Run

Direct kernel ELF boot is convenient for CPU and firmware debugging, but it
skips the file-loader's peripheral handoff:

```sh
./wremu -c images/wrcard.img images/grifo.elf
```

Use the serial-FLASH boot chain for the live panel. It performs the real LCD
controller initialization before loading the kernel:

```sh
./wremu -g -N 3,1000000 -e ../samo-lib/mbr/flash.rom \
    -c images/wrcard.img
```

With a window, the device initially appears powered off. Press `P` or click
the power symbol. Click the touch panel, use `1`, `2`, and `3` for the random,
search, and history buttons, and use `Q` or `Esc` to quit.

Typical boot output is:

```text
load: kernel.elf
Grifo starting
init starting
starting wiki app
VERSION: 20260823
```

Typing `LOVE` on the keyboard returns article titles from the attached wiki
data.

### Useful options

| Option | Purpose |
| --- | --- |
| `-g`, `-S N` | Open the SDL window at scale `N` (default 3). |
| `-c FILE` | Attach a card image, plain FAT32 or MBR-partitioned. |
| `-R` | Keep the card image read-only. |
| `-e FILE` | Attach serial FLASH and use the hardware boot path. |
| `-n N` | Stop after `N` target cycles/instructions; the GUI defaults to unlimited. |
| `-s` | Trace grifo syscalls with call sites and return values. |
| `-K cycle,TEXT` | Type text on the on-screen keyboard. |
| `-T x,y,cycle` | Tap a pixel; repeatable, up to eight. |
| `-G x,y0,y1,cycle` | Drag vertically; repeatable, up to eight. |
| `-N code,cycle` | Press random/search/history/power (`0`-`3`); repeatable, up to eight. |
| `-b ADDR`, `-W ADDR` | Break on execution or a write. |
| `-D ADDR -L N -O FILE` | Dump target memory. |
| `-m` | Trace unclaimed MMIO accesses. |
| `-P` | Report retired-opcode counts. |
| `-H` | Profile executed addresses. |
| `-X ADDR[,NAME]` | Count entries, report the longest gaps, and list the eight most frequent callers (the return address at entry; resolve with `addr2line`). |
| `-Y A,B`, `-y M,N` | Limit profiling by address or guest-time interval. |
| `-F FILE` | Write non-empty profile buckets: address, instructions, MCLK cycles, cycles waiting for the fetch, SDRAM row activations. Buckets cover 2 MB of SDRAM and, separately, the internal RAMs, so code in A0 RAM or IVRAM is never confused with the kernel or application at the same SDRAM offset. |
| `-Z ADDR` | Rebase the scripted input timeline on the first hit of `ADDR`. |

`WREMU_MODEL=name=value,...` overrides the fitted timing parameters listed
under "Calibration" for an experiment; the summary's `--- model` line shows
the set in use. `WREMU_SDRAM_TIMING=tRP,tRAS,tRC` (clocks) and `WREMU_SDRAM_AURCO=N`
replace every firmware write of the SDRAM controller's timing and refresh
registers, so one image can be timed under the boot loader's stock values
(`4,8,15` and `0x8c`) or the kernel's retimed ones (`2,3,5` and `0x1c0`).
`WREMU_SUSPEND_DIV=N` shortens the firmware's 120-second suspend interval
for testing without modifying the guest. `WREMU_HOLD_MS=N` changes how long
scripted taps and presses are held before release (default 33 ms).

The window has its own input path (SDL events, wall-clock timers, a power-on
reset when the device is switched on), so a bug seen only with the mouse may
not reproduce under `-T`/`-K`. `WREMU_GUI_CLICKS="x,y,at_ms[,hold_ms];..."`
pushes synthetic left clicks through that path at wall-clock milliseconds
after the window opens, in panel pixels; an item of the form
`d:x,y0,y1,at_ms[,ms]` is a drag from `y0` to `y1` taking `ms`. Combine with
`-g -N 3,1000000` to switch the device on and `-n` to end the run and write
`screen.pgm`. Scripted `-N` presses are timed in guest cycles, which advance
at roughly a third of wall-clock rate while the window idles.
`WREMU_TOUCH_TRACE=1` logs each packet the window hands to the touch panel
and each scripted drag step, and `WREMU_WALLCLOCK=1` gives a headless run the
window's wall-clock tick. `WREMU_DRAG_MS=N` spaces the sixteen steps of a
scripted `-G` drag N units apart instead of 5 (the unit is the `-G` cycle
count, which is instructions retired, so the guest time depends on the
firmware's work per instruction). `WREMU_LCD_TRACE=1` logs every write of the
LCD controller's framebuffer address with the MCLK time; while the firmware
scrolls by repointing that address, this is one line per displayed frame and
the cleanest way to measure scrolling frame rate, since it costs the guest
nothing (a serial trace inside the firmware stalls it for tens of ms a line).

### Profiling and timing

Use `-Z` to align scripted input to a guest milestone. Absolute `-T`/`-K`
cycle numbers do not produce comparable interactions when two firmware builds
reach the UI at different rates. Use `-Y` or `-y` to exclude idle polling
from profiles.

A `-Y` window also reports the SDRAM controller's work inside it: wait
cycles, refreshes, queue hits and misses, and row activations by bank, by
access kind per bank, and by the kinds of the two accesses on either side of
each activation (`--- window sdram`, `--- window activations by bank`, and
the following lines). With `WREMU_ROWHIST=1` the window also lists the 24
most activated 1 KiB rows and the 24 most frequent row-to-row transitions
within a bank (`--- window activations by row` and `by row pair`), which
tells which objects alternate; a same-row pair is a bank closed by refresh
and reopened. `WREMU_ROWTRACE=0xADDR` prints the PC and access kind behind
the first 48 activations of that address's row inside the window, after
skipping `WREMU_ROWTRACE_SKIP` of them. `WREMU_WINDOW_REPEAT=1` reopens a
`-Y` window at every later hit of its start address and adds the intervals
up, for a phase that recurs once per block. The summary's `--- dstram stack`
line gives the lowest stack pointer seen inside DSTRAM, where the ZIM reader
runs its decoder loops on a private 1 KB stack. The `-F` profile carries cycles and activations per
2-byte bucket, so `addr2line` on an unstripped link turns it into a
per-source-line cost that includes memory stalls. Cycles, not instruction
counts, are what to look at on this core: the ZIM article load below runs at
3.5 to 4.7 cycles per instruction, and an instruction-count profile of it
ranks the wrong lines. The CPU has no cache; the model charges a row change
for every move to another 1 KiB row, closes every bank at each auto-refresh,
and treats the two-slot 16-byte instruction queue as the only fetch
buffering. A row register belongs to the access port (fetch, data, DMA) and
not to the bank, so two data addresses evict one another however far apart
they are: the device reads a pair 4 MB apart at the same cost as a pair a
kilobyte apart. That is measured for a pair of read streams and no more --
a 512 KiB *copy* on the device does care about the separation, which nothing
here explains. See
[the memory-copy benchmark](tools/mem_dma_bench/README.md).

ZIM reader article load, Simple English `Cat`, retrieval to render entry,
run from the repository root with a card from `zim/make-card-image`:

```sh
./emulator/wremu -R -c /tmp/card.dmg samo-lib/grifo/grifo.elf \
    -T 40,36,100000000 -K 300000000,CAT -T 30,40,500000000 \
    -Y 0x<retrieve_article>,0x<render_article_with_pcf> -F prof.txt -n 1200000000
```

Use addresses from the matching `zim/zim.map`; they move with every build.
Booting `grifo.elf` rather than `-e samo-lib/mbr/flash.rom` skips the
file-loader's LCD initialization, which is over long before the window
opens, and is what the benchmark harness does. The first tap picks the
reader on the launcher menu, so `-Z` cannot be used. Scripted tap release is
delivered when the emulator next idles; a faster build can therefore render
extra frames while the scripted touch remains held.

### Calibration

The defaults in `src/model.c` were refitted on 2026-09-13 against a 32 MB
WikiReader running the NuttX port's `ubench`, `bench` and `cardb`
(`nuttx/overlay/apps/system/bench/*-device.txt`), booted through grifo so
that the machine state matches the device's. Parameters can be overridden
with `WREMU_MODEL=name=value,...`.

| Parameter | Default | Meaning |
| --- | ---: | --- |
| `branch_taken` | 5 | taken branch cycles with an SDRAM target |
| `branch_taken_iram` | 6 | taken branch cycles with an internal-RAM target |
| `branch_bubble` | 4 | MCLK an *undelayed* `jp`/`jpr` owes the fetch path, as a floor on the next fetch's wait |
| `call_extra` | 2 | extra cycles a call or return pays for the queue it discards |
| `iqb_first` | 1 | extra half-MCLK before an instruction-queue fill |
| `iqb_word_gap` | 0 | extra half-MCLK between words of that fill |
| `dq_extra` | 0 | extra half-MCLK on a data-queue fill |
| `dq_hit` | 1 | ...and on a data-queue hit |
| `dq_entries` | 1 | 32-bit words the data queue holds; the device measures one |
| `act_overlap` | 1 | a row activation may proceed while the data bus is busy elsewhere |
| `dq_iram_extra` | 2 | ...in SDCLK, for a data read issued by code running from internal RAM |
| `wr_ticks` | 9 | half-MCLK of bus occupancy per written halfword |
| `write_post` | 1 | a store retires into the controller's buffer; 0 blocks the CPU |
| `wr_rd_turn` | 0 | extra half-MCLK for an SDRAM read after a write |
| `sdclk_half` | 4 | half-MCLK per SDCLK; the device divides MCLK by two |
| `row_ports` | 1 | the open row belongs to the access port, not the bank |
| `row_change_extra` | 1 | extra half-MCLK on a row change |
| `cas_first` | 0 | the first halfword lands on the CAS cycle, not after it |
| `mmio_wait` | 8 | extra MCLK on a CPU access to a peripheral register |
| `iram_word_fetch` | 1 | the internal bus is 32 bits: charge the fetch that starts a word |
| `dma_extra` | 30 | extra MCLK cycles per HSDMA or IDMA transfer |
| `sd_read_latency` | 12000 | cycles from a read command to the data token |
| `sd_init_latency` | 0 | cycles from the first ACMD41/CMD1 until the card becomes ready |
| `sd_read_gap` | 0 | cycles before each subsequent CMD18 block token |
| `sd_write_latency` | 47000 | programming busy cycles after a written block |
| `iram_fetch_wait` | 0 | extra cycles per A0 RAM instruction fetch |
| `ivram_fetch_wait` | 1 | ...and per fetch from IVRAM or DSTRAM |
| `iq_row_evict` | 1 | a queue line dies when its row is closed; 0 prices a page crossing |
| `iq_lookahead_seq` | 1 | the core's run-ahead only runs while the fetch stream is sequential |
| `bank_floors` | 1 | tRAS and tRC belong to the physical bank, not to the row register |

`ubench sdclk` measured the SDRAM clock on the device on 2026-09-16 by
sweeping one controller field at a time — a slope, which the controller's
fixed overhead drops out of — and **an SDCLK is one MCLK**, not the two
that had been fitted out of a single row-change cost. Halving `sdclk_half`
halved every controller cost denominated in it, and the deficit that left
was not a blur: 10.7 MCLK on each sixteen-byte queue line and 1.5 MCLK on
each data read, which `iqb_word_gap` and `dq_extra` now carry. The refit
that followed is below; `mmio_wait` and `wr_ticks` moved with it.

Against the device, in rising order of how much the workload resembles real
code. The `ubench` rows are current; **everything from `ramspeed` down was
measured before the clock was corrected and is stale** until those are run
again.

| Workload | Result |
| --- | --- |
| 90 `ubench` loops | 0.0898 RMS log error, 64 within 10%, 87 within 20% |
| the `rowrate` sweep | 7.39 MCLK a row change on the device, 8.87 here |
| 19 `ubench bcs` fetch-window loops | 0.103 RMS log error, from 0.372; see below |
| `ramspeed` | memcpy 0.78-0.87x, memset 1.09-1.11x |
| 280 `arch_libctest` throughput points | median 0.936x, mean 1.009x |
| CoreMark / Dhrystone | 0.96x / 0.78x, same binary both sides |
| Whetstone | 0.99x |
| `cardb`, eight points of a 512-32768 byte sweep | within 3.1%, mean 1.007x |
| ZIM article load, Cat and Tokyo | 1.027x and 0.991x |

All eight of NuttX's C library suites pass identically on both — 196 checks,
same output — which is the only check here of the emulator's ISA *semantics*
rather than its timing. The device runs them in one go with `libct`.

The `ubench` set is 69 loops now; the 0.139 figure covers the 45 that
predate the fetch-window probes, and the 24 added for those are counted
separately because they were chosen to sit where the model is worst.

### What the residual is

The residual is in instruction fetch, and it has a measured rule.

**A loop body is resident on the device while its offset inside the enclosing
16-byte line plus its size is at most about 27 bytes, and not otherwise.**
Sixteen `ubench` loops sweep four sizes across four offsets with one load and
one store throughout (`ubench bcs`), and sorted by `offset + size` the split
is total: every fast case ends at 26 or less and every slow one at 30 or
more.

| size | off 0 | off 4 | off 8 | off 12 |
| ---: | ---: | ---: | ---: | ---: |
| 10 | 16.59 | 16.59 | 16.59 | 16.59 |
| 18 | 20.60 | 20.60 | 20.60 | 57.22 |
| 26 | 24.32 | 60.80 | 59.65 | 60.08 |
| 34 | 63.80 | 64.09 | 66.09 | 77.39 |

Neither size nor alignment alone predicts any of that, which is why it took
so long to see. A 26-byte body is fast at offset 0 and 2.5x slower four
bytes along; an 18-byte body is fast until offset 12; a 10-byte body is fast
anywhere. It also explains the loops that looked contradictory: `ub_alu` is
22 bytes at offset 2, ending at 24, so it is fast at every position the
boundary table tries — that table moves whole functions by multiples of 16
and so never varies the offset at all.

The model charges by how many lines a body spans, which is `offset + size`
up to 32. That reads three of the sixteen 2.5x fast and most of the rest
1.17x slow, and it is why `stpncpy`'s 22-byte byte loop reads 3.2x fast in
`arch_libctest`.

**`iq_lookahead` implements the rule, is on, and runs only while the fetch
stream is flowing.** Six bytes of lookahead reproduces the rule exactly — a
body ending past about 27 pulls in a third line before the branch takes it
back, and the third evicts the first — taking the sweep from 0.372 to 0.104
RMS log error with every fast/slow call correct.

The run-ahead is the core's, not the controller's: II.4.2.2 and II.4.2.4 of
the chip manual have the SDRAMC filling a slot only on a miss, while the CPU
is specified as "internal 2-stage pipeline and **4 instruction queues**", up
to eight bytes in front of what is executing. The C33 core manual's 5.14.2
says what happens at a branch — the delayed forms exist precisely because
"the instruction that follows it has already been fetched", so the undelayed
forms discard that fetch and the stream restarts from the target.

`iq_lookahead_seq` (default 1) is that restart: no run-ahead on a fetch more
than four bytes from the last one, because a fetcher that has just been
redirected is not yet ahead of anything. Without it, `ubench`'s `callret`
prefetched the line past its callee on every one of its eight call/ret pairs
a pass and cost 543.15 cycles against the device's 171.00 — a 28-byte loop
that fits the two-slot queue entirely. With it, 153.90. Across the loops
whose comparison does not depend on where the heap put a buffer, RMS log
error falls 0.2657 to 0.0673, and CoreMark, Dhrystone, `memcpy` and `sdbench`
each move slightly closer to the device.

Before that, six other mechanisms were fitted and refuted, each by a loop
the previous one did not cover; the best fit for the loops among them
(0.097 RMS) put CoreMark at 1.10x and Dhrystone at 1.17x. A separate fetch
bus is structurally wrong whatever it fits: figure II.4.2.1.1 puts both
queue buffers behind one address register, one queue-buffer controller and
one SDRAM interface. II.4.3 (Bus Arbiter) covers only LCDC/DMA/CPU/SRAMC
priority and says nothing about how a fill and a data access share that
interface, which is the part still being guessed at.

**For firmware, not just for the model:** keep a hot loop body within 26
bytes of the enclosing 16-byte boundary. Aligning a loop is neither
necessary nor sufficient — a small body is fine anywhere and a 34-byte body
is slow even at offset 0. `-falign-loops` cannot express it.

`iqb_first` is a deliberate compromise at 1. Every `ubench` loop fits the
instruction queue and so measures the fill of a line refetched every pass,
which wants a larger value; Dhrystone has a real code footprint and a queue
that misses constantly, and wants zero. Do not fit it against either alone.

A loop that straddles a 1 KB page costs the device 3.2x what the same loop
costs anywhere else. In aggregate that is small -- CoreMark loses 0.6%
between the eviction modelled and not modelled, so that much of it goes on
code lying across page boundaries, although 21% of its queue misses involve
one. It is a lottery rather than a tax: most loops never straddle, the ones
that do pay 3.2x, and whether any of them is hot is a property of one build.
`WREMU_MODEL=iq_row_evict=0` prices it for any other workload.

That makes it a thing to find rather than to prevent. Aligning every loop is
the wrong trade: `-falign-loops=32` works on this backend but does not stop a
1 KB crossing, and `-falign-loops=1024:30`, whose max-skip would pad only the
loops that need it, emits a bare `.align 10` because the C33 backend has no
ASM_OUTPUT_MAX_SKIP_ALIGN -- every loop in the image padded to a kilobyte.

The mechanism is the instruction queue: the hardware cannot keep a line whose
row has been precharged to reach the other, so a queue line is evicted when a
fill activates another row of the same bank. With that, the cliff lands
within 2% -- 3.22 seconds against 3.16 -- and the seven of eight positions
that were already right are unchanged.

### What internal-RAM code costs, and the charge that went missing

The `ubench` loops that price internal RAM are small and self-contained, so
for a long time nothing in the tree ran mostly out of A0 RAM and the IVRAM
window at once. The rv32 interpreter does, and it found two errors of
opposite sign. Its four placement builds run one guest workload with more
and more of the interpreter in internal RAM, and `riscv/fit-model.py` scores
them against the device's own reports:

| what runs where | device / model |
| --- | ---: |
| all in SDRAM | 0.905 |
| code in A0 RAM | 0.970 |
| code and machine state in A0 RAM | 1.047 |
| assembly hot path, state and dispatch table internal | 1.093 |

**`dq_iram_extra` had become dead code.** It charges a data read issued by
code that is not itself coming over the SDRAM bus, it was fitted at 2 SDCLK,
and the row-model refit (`eb75315e`) deleted the line in `schedule_read`
that applied it while leaving the parameter, this table's entry, the
`model_describe` output and the `WREMU_MODEL` override all in place. The
symptom was silence: sweeping it changed no answer at all. Restored, the two
internal-RAM-heavy builds go from 1.047 and 1.093 to **0.999 and 1.030**,
overall RMS log error from 0.0927 to 0.0877, and the 117 checks in
`make test-sdramc test-isa test-dma` still pass.

Two things it does not fix. Code running **from SDRAM is modelled about 10%
too expensive**, near-uniformly across kernels -- the 0.905 row, which no
internal-RAM parameter touches. And the charge has the wrong shape: it is
right where such data accesses are few (0.999, 1.030) and too big where they
are many, which is the middle build's 0.884. `iqb_first=0` and
`branch_taken_iram=12` each take a little more off the residual (0.0794
together), but both were fitted against `ubench` and neither should move
until `ubench` has been rerun on the device to check what that would cost.

### What an unconditional jump costs the fetch path

`ubench`'s `br32` is `long` with half its adds replaced by an undelayed `jp`
to the *following* instruction: the same 67 instructions in the same 138
bytes, differing in nothing but that. The device runs it in 312 cycles
against `long`'s 183. The model ran it in 193.

The queue had already fetched the line the target sits in — a jump two
bytes ahead never leaves it — so serving the fetch cost nothing, and the
three execute cycles the manual gives `jp` hid under a fetch schedule the
device does not get to overlap. `branch_taken` never applied: it is reached
only from the conditional branches, and `jp`/`jpr` take the flat figure.
So the conditional case was priced and fitted and the unconditional one had
never been priced at all.

`branch_bubble` is a floor on the next fetch's wait rather than an addition
to it, because what the fetcher had run ahead and read is for an address the
program is no longer going to. At 4 MCLK `br32` lands on 318 against 312 and
**every other loop in the file is unchanged to the cycle** — `alu sdram`
0.990, `long` 0.992, `f128` 1.000, `alu ivram` 1.000.

It applies only to the undelayed forms. A delay slot exists to be executed
while the fetch path restarts, so `jp.d` has already paid for the bubble
with the instruction after it; charging it too costs the rv32 interpreter's
C builds 0.884 → 0.808, since a compiler fills delay slots and an
interpreter is mostly jumps.

Two other shapes were tried against the same measurement and rejected. A
flat charge on *every* control transfer breaks the conditional case that
already fits — `alu`'s one taken `jrne` a pass is priced by `branch_taken`
and wants nothing more. Evicting the queue on a branch does not fix `br32`
at all, because its 138-byte body thrashes the queue either way, and costs
`alu` 15.15 → 39.60 against a device that says 15.00: a resident loop then
refetches itself every pass.

The cost is small and worth naming. Scored against the rv32 interpreter's
four builds the RMS log error goes from 0.0877 to 0.0902, because the two
middle builds were already too expensive for an unrelated reason — the
data-movement overcharge below — and this pushes them further the same way.
The build that ships, the assembly one, improves: 1.030 to **1.009**.

### What is from the manuals and what is fitted

Two documents: `s1c33.pdf`, the 181-page C33 PE core manual, and
`id001557.pdf`, the 1015-page S1C33E07 technical manual. It is worth being
explicit about which half of this model comes from them, because the fitted
half is where the errors live and the grounded half is not up for
negotiation.

**Structure, from the manual, and the model matches it.** IQB is "2 slots ×
8 × 16 bits" on 128-bit boundaries, filled 8 halfwords at a time on a miss,
"the two slots used alternately" (II.4.2.2) -- which is the model's two
16-byte lines and its round-robin `iq_next`. DQB "consists of two-stage
16-bit buffers ... Buffer 0 and Buffer 1 correspond to two-burst reading",
one 32-bit line in two halves, which is `dq_entries = 1` (II.4.2.3); a
second entry was tried and the device refused it independently. DQB is
inactive for instruction fetch while IQB is on and inactive for every write;
a write flushes a matching IQB or DQB entry (II.4.2.4). `jp` is three
cycles and `jp.d` two (core manual 5.14.2), which is what `cycle_cost()`
charges -- and the reason given for the delayed form, that "the instruction
that follows it has already been fetched", is the documented basis for
`branch_bubble` applying only to the undelayed one. tRP, tRAS and tRC come
from the configuration register, the refresh counter is 12-bit, and the bus
arbiter's priority is LCDC, DMA, CPU, SRAMC (II.4.3).

**Fitted, with no figure in either manual.** Every overhead:
`branch_bubble`, `iq_lookahead`, `iqb_first`, `dq_extra`, `dq_hit`,
`row_change_extra`, `call_extra`, `mmio_wait`, `iram_fetch_wait`,
`ivram_fetch_wait`, `dq_iram_extra`, `act_overlap`, `cas_first`,
`iram_word_fetch`, `wr_ticks` and the card latencies. The manuals give
structure and instruction cycles; they do not price a wait state on a
system bus, and that is most of what this model is.

Two of the fitted ones are worth naming against the documentation.
`iq_lookahead = 6` has no basis in the SDRAMC chapter -- the manual
describes demand fetching, not a run-ahead prefetcher -- but the CPU summary
says the core has "a 2-stage pipeline and 4 instruction queues", four
16-bit entries being eight bytes of run-ahead, and six is what the device's
residency rule fits. The mechanism is documented in the core's chapter and
the value is measured. `branch_bubble = 4` sits on top of the manual's three
cycles for `jp` and is not in either book: the device charges four more than
an `add` for a jump to the next instruction, where the manual's figures
account for two.

**Two places the model contradicts the manual, both on measurement.**
`write_post = 1` buffers one store, where II.4.2.4 says "the internal wait
signal input to the C33 PE Core is asserted until the SDRAM interface has
finished writing to the SDRAM" -- no posting at all. The device says
otherwise: it copies a word at a time faster than four at a time, which
only happens if a store retires before the bus has taken it, and
`write_post = 0` takes the rv32 interpreter from 0.0793 to 0.0947.
`row_ports = 1` gives the open row to the access kind rather than the bank,
where the manual supports "max. 4 SDRAM banks and bank active mode"; the
device charges the same for two addresses a kilobyte apart and four
megabytes apart, which under the geometry table are different banks. That
geometry is separately suspect -- both 32 MB boards behave as 4 MB banks
where the table says 8 MB -- and the discrepancy is unexplained.

### Whole programs

Loops and one interpreter say what the model does with cycles; these say
what it does with a program. `nuttx/` has the suite and `make bench` runs
it both sides -- the device command `bench`, then
`run_benchmarks.py --compare`. Emulator over device, so above one is the
model running fast:

| | ratio |
| --- | ---: |
| CoreMark | 0.98 |
| Dhrystone | 0.92 |
| Whetstone | 0.98 |
| ramspeed memset, internal | 1.08 |
| ramspeed memset, system | 1.12 |
| ramspeed memcpy, internal | 0.84 |
| ramspeed memcpy, system | 0.87 |
| sdbench write / read | 1.07 / 1.04 |

0.090 RMS log error over the eleven figures, worst case 0.84x.

The three CPU benchmarks land within 8%, and that is the check the fetch
lookahead was originally reversed on: enabling it used to take CoreMark
from 1.05 to 0.87 and Dhrystone from 1.01 to 0.92. With activations
overlapping the data bus it is on and they are 0.98 and 0.92 -- the damage
it did was contention that no longer exists.

Whole programs also found what the loops could not. `act_overlap` has to
clamp the transfer to the bus in `schedule_write` as well as
`schedule_read`, and for a while it only did the second: a store began the
moment it was issued however busy the bus was, so a run of them never
filled it. `ubench` called that 12% on `storeseq`, because `storeseq` is a
loop with other work in it. ramspeed's memset is nothing but stores and
called it **2.11x**. Clamping both puts memset at 1.08, takes the suite
from 0.236 to 0.090 RMS, and adds eight loops to `ubench`'s 10% band.

What is left is memcpy, 0.84 and 0.87 -- the model is slow where it was
fast. `ubench`'s `copyw` is 0.722 and looks like the same thing; it is not,
and the two have to be separated.

Turning the fetch lookahead off decides it. `copyw` goes 0.722 -> 1.019 and
`copyfar` 0.762 -> 1.070, so those two really are the lookahead's doing: a
body that already thrashes the two slots is made worse by pulling a third
line. But memcpy goes 0.84 -> **0.81**, slightly *worse*, so whatever ails
it is not the lookahead, and a hand-written four-word copy loop does not
predict what libc's memcpy does. Meanwhile CoreMark goes 0.98 -> 1.20.

The lookahead is better on all three measures and stays on:

| | on | off |
| --- | ---: | ---: |
| whole-program RMS | **0.090** | 0.119 |
| ubench RMS | **0.206** | 0.259 |
| ubench within 10% | **51/86** | 49/86 |
| rv32 RMS | 0.0793 | 0.0785 |

So the remaining error is the one that has been there all along and is now
much smaller: loops alternating two data streams, modelled 20% too
expensive -- `st2` 0.781, `copydisp` 0.755, memcpy 0.84 and 0.87 -- which
`act_overlap` took from 0.67 without finishing. `copyw` and `copyfar` are a
separate and smaller debt, owed to the lookahead, and priced above.

### Fetch and data on one bus: activation overlap

The largest error the model had was a cluster of loops charged 20 to 34%
too much, and it is now understood and mostly gone. What follows is how it
was found, because the route matters more than the answer.

`ld2` and friends -- two streams read alternately -- looked like a data-path
problem, and four data-path explanations were tried and refuted (below).
Five loops then took `ld2` apart one property at a time and the device said
it was none of them: not walking (`ld2fix` 0.782 against `ld2`'s 0.776), not
the separation (0.789 to 0.829 from one row to four megabytes), not the
access width (`ld2` and `ld2w` come back at 254.06 apiece, identical to the
cycle). One stream instead of two is 0.983.

What placed them was sorting on two properties together:

| | data changes rows | data stays in one row |
| --- | --- | --- |
| **body outruns the fetch window** | 0.70-0.83 | 0.98-1.01 |
| **body stays resident** | 1.026 | 0.99-1.00 |

Fifteen of sixteen loops, and neither property does anything alone.
`rowthrash` and `rtbig` then tested it directly: they differ in nothing but
twelve adds padding the body past the window, and the device charges 24.90
cycles for them where the model charged 75.75 -- 2.7 cycles a code byte,
where a fetch-only loop measures 1.33 in both.

The cause was in `select_row`. Every access began at `max(bus_free, now)`,
so a row activation queued behind whatever the data bus was doing. Real
parts do not work that way: ACTIVATE is a command, tRCD elapses inside the
bank, and another bank may be moving data throughout. `act_overlap` starts
the activation when the request arrives and serialises only the transfer.

| | before | after | device |
| --- | ---: | ---: | ---: |
| `st2` | 0.697 | **0.856** | |
| `st2skew` | 0.665 | 0.805 | |
| `ld2fix` | 0.782 | 0.869 | |
| `copydisp` | 0.720 | 0.823 | |
| `rtbig` | 0.779 | 0.842 | |
| `loadseq` | 0.993 | 1.000 | |
| `rowthrash` | 1.026 | 1.036 | |

ubench goes from 0.273 to 0.261 RMS log error with six more loops inside
10%, and the rv32 interpreter's four builds from 0.0902 to 0.0793 -- its
all-SDRAM build from 0.905 to **1.008**. It costs `storeseq`, 1.012 to
1.120: writes now retire too cheaply.

One thing the overlap must not do is skip an array-wide wait. A refresh
precharges every bank and self-refresh exit wakes the device, and nothing
may activate through either however idle the data bus is; `array_free`
tracks that separately from `bus_free`. Without it the self-refresh exit
test drops from 23 cycles to 19, which is how the omission announced
itself.

### The data-movement overcharge, and four things it is not

The largest error left is a cluster of loops that move data, and only those:
`st2skew` 0.665, `st2` 0.697, `copydisp` 0.717, `ld2w` 0.732, `ld2` 0.776,
`mix16` 0.795 — the model charges them 20 to 34% too much. The rv32
interpreter's SDRAM-resident builds say the same thing at 0.905 and 0.871,
so it is two independent workloads with one sign.

The signature is sharp and narrows it a long way. Single-stream loops are
right — `loadseq` 0.993, `storeseq` 1.012, `copyw` 1.014 — and so are loops
that alternate between two *fixed* addresses, `rowthrash` 1.056 and
`bankpair` 1.015. What is overcharged is alternating between two streams
that walk.

Four explanations have been tried against the measurements and are not it:

* **A second data-queue entry.** The obvious reading of "two streams", and
  wrong twice: `rowthrash` collapses to 23.10 against a device saying
  145.95, and the target loops do not move at all — `st2` stays at 0.697.
  The device holds one word, which that loop now says outright.
* **`row_change_extra=0`.** Moves the cluster a little (`ld2` 0.776 →
  0.809) and takes `rowthrash` to 1.124 and `bankpair` to 1.079 for it.
  Net zero, and the parameter's own comment predicted this: loadseq wants a
  cheaper read and rowthrash a dearer miss.
* **`write_post=4`.** Fixes the same-address store bursts — `store sdram`
  0.877 → 0.972, `storeb` 0.881 → 0.972 — and breaks the walking one,
  `storeseq` 1.012 → 1.273. `st2` again does not move.
* **`dq_hit`.** No effect at any value on the rv32 workload.

So it is not queue depth, not the row-change price, not write buffering.
Whatever it is, it distinguishes a stream that walks from one that stands
still, which no parameter here currently does.

Five loops in `ubench` take `ld2` apart one property at a time to find out
which -- same sixteen byte reads a pass, same loop, same pass count, so a
difference is the property and not the shape. The model says all five cost
within 8% of each other, so **any spread the device shows is the mechanism**:

| loop | model | `ld2` with |
| --- | ---: | --- |
| `ld2` | 327.30 | — (the device says 254.06) |
| `ld2fix` | 304.13 | both addresses standing still |
| `ld2mix` | 313.00 | one walking, one standing |
| `ld2near` | 327.30 | the streams one row apart, not 512 KB |
| `ld2far` | 320.43 | four megabytes apart, another bank |
| `ld1walk` | 115.30 | one stream instead of two |

The device answered: **none of them**. `ld2fix` is 0.782 against `ld2`'s
0.776, so walking is not it. `ld2near` through `ld2far` run 0.789 to 0.829
across a span of four megabytes, so the separation is not it. `ld1walk`, one
stream instead of two, is 0.983. And `ld2` and `ld2w` come back at 254.06
apiece -- **identical to the cycle** -- where the model charges the word read
a megahertz-cycle more for its second halfword.

Sorting every loop in the file by two properties does place them, though:

| | data changes rows | data stays in one row |
| --- | --- | --- |
| **body outruns the queue** | 0.70-0.83 | 0.98-1.01 |
| **body stays resident** | 1.026 | 0.99-1.00 |

Fifteen of sixteen. The model overcharges only where code must be fetched
constantly *and* the data accesses change rows -- contention between fetch
and data on the one bus, which it evidently serialises harder than the
hardware does. Neither property alone does anything: `rowthrash` alternates
two rows in a 32-byte body and fits at 1.026, `ld1walk` outruns the queue
with its data in one row and fits at 0.983. (`bankpair`, 34 bytes, only just
misses and sits at 1.015.)

`ld2small` and `rtbig` cross the pair over and are the test of it:
`ld2small` is `ld2fix` with a body small enough to stay resident and nothing
else changed, and `rtbig` is `rowthrash` with adds padding its body past the
window and its accesses untouched. The model puts them at 65.49 and 213.90.
If the reading is right the device says about 65 for the first and about 171
for the second; if it is backwards, they come back the other way round and
the fault is in the number of accesses rather than the contention.

### The translator templates, which miss the other way

`riscv/jit_probe.s` is twelve more measurements on the device, and they are
worth reading against the section above because they carry the opposite sign.
They are hand-written translations of guest basic blocks, run from SDRAM and
from A0 RAM; `riscv/rvjit-device.txt` is the device's answer and
`riscv/README.md` the reading of it. Device over model:

| template | SDRAM | A0 RAM |
| --- | ---: | ---: |
| `alu_reg` | **1.000** | 0.970 |
| `alu_mem` | 1.040 | **1.193** |
| `ld_free` | 1.145 | 1.003 |
| `ld_check` | 1.179 | 1.000 |
| `copy_reg` | 1.074 | 1.034 |
| `copy_mem` | 1.127 | 1.102 |
| `exit_none` | 1.038 | 0.972 |
| `exit_link` | **0.913** | — |
| `exit_hash` | 1.071 | — |

The interpreter's all-in-SDRAM build says SDRAM-resident code is charged
about 10% too much, so these were expected under the model. Every one of them
is over it.

What separates the rows is not the code but what runs underneath it.
`alu_reg` in SDRAM is a pure instruction stream with no data access at all,
and the model gets it exactly: 3.50 against 3.50. `ld_free` and `ld_check`
are the same code shape walking a data stream beneath the code stream, and
they are 14 and 18% dear -- while the same loads issued from A0 RAM, where
only the data comes off the bus, are exact to a thousandth. So the error is
in fetch running against data, and it appears as an *under*charge where the
two-stream loops in `ubench` are an overcharge. Those alternate two *data*
streams; these alternate a code stream with a data stream. One bus, two
readings, opposite signs -- which says the contention term is wrong in shape
and not merely in size.

`exit_link` is the exception that names the other error. It is eight blocks
chained by patched jumps, so its fetch stream is nothing but broken runs, and
it is the one row the device beats the model at -- 0.913, against 1.038 for
the same eight blocks run straight through as `exit_none`. The interpreter's
all-in-SDRAM build says 0.905 and a dispatch loop is jumps all the way down,
so two independent workloads now put the overcharge on the *jump* rather than
on SDRAM-resident code in general. `branch_bubble` and the queue restart under
it are about a tenth too dear; sequential fetch is if anything slightly cheap.

`alu_mem` in A0 RAM is the one internal-RAM row that misses, by 19%, and it
is the only template whose data is also internal: twenty-one `ext`-displaced
loads and stores into the register file in A0 RAM across fifty-two
instructions, nothing touching SDRAM at all. Fifteen cycles a pass, about
0.7 of a cycle an access, that the model does not charge for a dense stream
of internal data from internal code. The interpreter does exactly this on
every guest instruction it executes.

### Compare against the device, not against a direct boot

Four traps, all the same shape: the device is never in the state a direct
boot starts in.

- An application runs on the timings `SDRAM_retime()` leaves, not the
  loader's.
- A guest that never programs the SDRAM controller was once modelled with no
  memory system at all, and ran at about a cycle an instruction.
- A direct ELF boot skips grifo's PLL setup, so the CMU reports OSC3's
  48 MHz while a guest configured for the real 60 divides its tick from that,
  and every guest-measured second comes out a quarter long.
- Adding `ubench` loops moves the device's own CoreMark and Dhrystone --
  Dhrystone was 15232 on one build and 13708 on the next -- so a stale device
  reference can make a model that is 0.3% out look 10% out.

Hence the benchmark harness boots grifo off a card, and `bench` is re-run on
the device whenever the image changes. `bench` takes benchmark names and
`ubench` takes loop names, so re-running the two that decide a model change
is half a minute on the device rather than five -- without that, the
temptation is to judge against a reference from a different binary, which is
how the fetch lookahead nearly got accepted and nearly got discarded on the
same day. Historical calibration data and its retired harness remain in Git
at `7aa4ee84`.

### The card

`sd_read_latency` and `sd_write_latency` are fitted from `cardb`, which
sweeps the block size so a fixed cost per operation can be told from a cost
per byte. Fit them on a filesystem with one sector per cluster: the driver
then issues a command per 512 bytes, which is what makes the per-command cost
visible at all. `sd_read_latency` sat at 60000 -- a millisecond a command --
through every earlier calibration, because the only card workload ever
measured was grifo reading 255 sectors at a time, where it is a rounding
error; at one command a sector it was most of the read time and the model ran
at 0.66-0.78 of the device. `sd_init_latency` and `sd_read_gap` are zero, and
are mechanisms for fitting a measured wait rather than measured defaults.

All four are MCLK cycles, 60,000 to the millisecond. Initialization polls
return idle until ready, streamed reads delay only the next data token, and
programming busy survives chip deselection.

Device read numbers move about 20% run to run where writes repeat to a tenth
of a percent. Do not fit reads tighter than that.

Match filesystem state as well as firmware before comparing boot times. A
generated FAT32 fixture with unknown FSInfo hints and no existing `dma.txt`
spent 647 ms creating its first diagnostic and 26 ms for the same step on the
next boot -- allocation work, not a card write delay. Give fixtures valid
free-cluster and allocation hints, and include the card's boot files,
directory order, existing logs and history. For an existing **synthetic**
fixture:

```sh
python3 emulator/tools/fat32_fixture.py /tmp/generated-card.img
make -C emulator test-sd-timing test-fat32-fixture
```

The tool bounds its FAT read and accepts regular image files only. Do not
apply it to captured physical metadata: the original allocation state is part
of the evidence. Even so, a fixture does not reproduce physical fragmentation
or deleted directory slots.

The summary separates executed instructions from fast-forwarded idle cycles:

```text
--- work: 219633709 instructions executed, 180366291 idle, 7973.0 ms guest ---
```

The `--- idle power: SD supply on ... ms, off ... ms ---` line divides
skipped HALT time by the board's P32 supply-enable state. It distinguishes
an idle card with power still applied from one whose supply is disabled;
P33 (the buffer enable), chip select, and SPI clock gating are not the
supply switch. It covers both headless and window runs, including DMA HALTs
in older firmware and the new full-clock timed event waits. SD-on time in
those short waits does not imply KEEP during deep suspend. This is
GPIO-state residency, not a current or battery
model; it excludes active execution and does not simulate card startup
current or card-specific standby behavior. See [power management](../zim/BATTERY.md)
for firmware comparisons and measurement limits.

Runtime firmware defaults to `-O2`: a full-FLASH stock-reader comparison
found `-Os` smaller but 3.2% slower in the modeled article interval, with
identical screens. That ordering has not been verified on hardware.
Boot stages retain `-Os` to fit their internal-RAM limits.

Kernel builds select the working DMA path with `SD_DMA=YES` (default), or
PIO with `SD_DMA=NO`. Production DMA completion polls with a bound: the
older HALT-based completion wait passed the emulator but never woke on
the real device.

`SD_DMA_TX=HSDMA` is the hardware-tested default for aligned word payloads.
Byte payloads still use RX-paced IDMA; `SD_DMA_TX=IDMA` selects the previous
word path for comparison. See [reader performance](../zim/PERFORMANCE.md).

## Hardware model

### CPU and memory

The core implements the documented C33 PE instruction set, `ext` composition,
delay slots, condition flags, special registers, strict natural alignment,
synchronous exceptions, debug exceptions, and prioritized interrupts. Cold
reset establishes TTBR `0x00c00000`, a PE IDIR type byte of `0x06`, and DBBR
`0x00060000`.

The generated decoder contains the union of Standard, Advanced, and PE
binutils tables. A separate PE-valid bitmap rejects the nine Standard
instructions removed from PE and the 18 Advanced-only operations. The
coprocessor forms decode, but execution is intentionally unsupported because
the S1C33E07 has no attached coprocessor; reaching one stops with a diagnostic
rather than inventing values for an unattached interface.

SDRAM timing is active after firmware programs `SDON`, MRS, and `APPON`.
The model derives geometry and waits from SDRAMC registers, tracks active
rows, queue buffers, refresh, and self-refresh, and puts CPU and DMA accesses
on a shared MCLK timeline. It conservatively serializes command and data
phases across banks instead of modeling all documented bank interleaving.

### Storage and DMA

The SD card operates in SPI mode. Character completion follows live `BPT`,
`MCBR`, and `SPI_WAIT` values and updates `BSYF`, `TDEF`, `RDFF`, and `RDOF`
at the scheduled event.

The card model implements the standard SDHC initialization and register
transactions used by Linux `mmc_spi`, including SCR, SD Status, switch
status, status, and CRC enable commands. Read payloads carry calculated
CRC16 values, and a successful ACMD41 changes the card from a loader's
legacy byte-addressed CMD1 session back to SDHC block addressing. Use
`--trace-sd` to log card commands without enabling the much noisier MMIO
trace.

The default read backend uses HSDMA3 for SPI RX and HSDMA2 to feed word TX.
Byte payloads and the IDMA comparison build use IDMA channel `0x24` to write
dummy TX data after RX completion. The model covers the dual-address
transfers used by firmware: request selection, priority, counters, address
updates, terminal enable clearing, descriptor writeback, clock gating, global
IDMA enable, and the terminal interrupt cause. A word block performs 128
HSDMA3 transfers plus either 127 IDMA transfers or 127 HSDMA2 transfers.
Byte mode uses 512/511 RX/IDMA transfers. Per-channel counters identify the
active pipeline in the summary.

Manual V.2.5 defines separate TXD and shift registers: TDEF/TXDE occurs at
shift start, while RDFF/RXDE occurs at completion. One TX word can queue
while another shifts; SPI_WAIT delays consuming that queued word. The model
keeps those events on the wire timeline, serializes DMA bus use, and retains
disabled-channel trigger flags until accepted or explicitly cleared. TXDE
and RXDE independently gate their request sources. SPI characters carry bytes
MSB first. Focused tests check byte/halfword/word payloads, the wire-duration
formula, TX terminal count preceding the final two RX completions, stale
requests, and disabled request sources.

The fitted `dma_extra=30` cost applies to both engines. It is an empirical
per-transfer allowance, not a measured arbitration waveform. Against the
device on a full-archive startup it puts file DMA wait at 2.119 s versus
2.183 measured, and total startup at 3.310 s versus 3.512.
Software-triggered HSDMA also supports single, successive and block transfers,
fixed/incrementing/decrementing addresses, and address restoration at the end
of a successive transfer or each block. Each unit performs a read followed by
a write through the SDRAM timing model. With unlimited sequential access the
CPU's bus access stalls for the whole trigger. Limited sequential access,
other hardware triggers, preemption within a DMA unit, and cycle-level
CPU/DMA arbitration remain unmodeled. Nonzero access-time limits on multi-unit
transfers are rejected rather than silently timed as unlimited transfers.

Memory DMA uses the separate, uncalibrated `dma_mem_extra` parameter (default
zero additional MCLK cycles per unit); the fitted SPI `dma_extra=30` is not
applied to it. The CPU-only internal-code data-read allowance is also excluded
from DMA accesses. The [memory-copy benchmark](tools/mem_dma_bench/README.md)
compares the same app on the model and physical hardware, with an unchanged
kernel. `make test-mem-dma` checks the added controller semantics independently.

ITC reset uses zero cause flags as a deterministic choice; the hardware manual
marks them indeterminate, and the device really does come up with
`FDMA=0x17` and no channel enabled, which cost the reader every memory DMA
until it was found. The C33 `test-zim-copy` suite exercises both set and
cleared causes, including initialization of an unconfigured channel and
preservation of a configured owner's completion. See the
[hardware findings](../zim/PERFORMANCE.md#hardware-findings-the-code-depends-on).

SPI interrupt-enable and receive-mask registers are retained, and the receive
mask is applied to received data. The summary's `spi config` line counts
control-register access while busy and disabling ENA with nonzero SPI_INT,
both forbidden by manual V.2.8. These checks diagnose invalid driver sequences
without assigning a malfunction to undefined operations. SPI CPU interrupt
delivery is not modeled. The C33 driver regression starts with the physical
loader's `SPI_INT=0x14` and requires both violation counts to remain zero.

The physical sector probe found a one-bit advance in the card's response for
each SPI ENA cycle, including unchanged-width CPU reads. The model reproduces
that observed net effect on disable when the card is selected, P67 is muxed
to SPI, and CPOL=0. Holding P67 at idle as GPIO prevents the advance. The
`spi clock` summary counts unclamped disables. The exact physical edge,
command/write bit assembly, and electrical pin-mux transients remain
unmodeled. See the
[hardware findings](../zim/PERFORMANCE.md#hardware-findings-the-code-depends-on).

Known divergence: the model lets the channel-3 terminal-count cause wake a
HALTed core. A real WikiReader (stock 2009 flash) never woke, and a kernel
that slept on that cause hung on the boot splash. The kernel now polls the
flag, which works on both. Do not rely on the emulator to tell you which
interrupt causes wake HALT.

### Peripherals

The current model includes:

- interrupt-controller priorities and read/modify/write register behavior;
- LCD control, framebuffer capture, and SDL touch/front-button input;
- EFSIF UART, SPI status, and serial FLASH;
- GPIO selection, polarity, edge/level input, and the board's power control;
- six T16 channels, prescalers, comparison buffering, interrupts, and the
  timer-0-to-timer-5 cascade used by firmware;
- CMU protection, clock gates, oscillator/divider decoding, and derived MCLK;
- ADC sweep/status behavior with physically plausible board values;
- the watchdog, including its clock gate, its NMI output, and the reset it
  asserts, which restarts the machine; and
- the S1C33E07 fixed chip-identification bytes.

Both ways of stopping the device are modelled, because the firmware uses both
and the difference is visible. `power_off()` toggles P63 until the supply
outside the chip drops the rails: headless, the run ends with
`stop reason: powered off`; with a window, the panel goes dark and the power
switch brings it back. `System_reboot()` arms the watchdog for a 100 us reset
instead, and that restarts the machine — memory cleared, peripherals reset,
boot image reloaded, running again from the entry point — rather than ending
the run, so a reboot can be followed to see whether the device came back.
`[watchdog reset]` is printed on each one and the summary counts them. A guest
that simply wedges reaches the same place after grifo's twenty seconds.

`-n` bounds the whole run, resets included, so a guest that resets in a loop
still stops. The counters the summary prints are per-boot, since a reset
clears them along with the peripherals; the `--- resets: ---` line says so and
gives the totals from the earlier boots.

The GUI uses wall-clock time for human input. Headless runs use deterministic
instruction- and event-derived guest time and fast-forward blocked intervals.

The RTC, unused alternate pins, unconnected inputs, unused timer waveforms,
and unused DMA triggers are intentionally out of scope because WikiReader
firmware does not use them.

## Validation basis

The model is checked against sources independent of the firmware being run:

- binutils disassembly agrees with the decoder on 65,605 instructions across
  the firmware images;
- all 90 opcode patterns extracted from the C33 PE Core manual agree with the
  generated table;
- instruction semantics, exception behavior, reset values, and modeled MMIO
  registers are covered by manual-derived focused tests;
- the watchdog reset restarts a bare-metal guest that arms it, in
  `make test-wdt-reset`: the guest marks each boot on the UART, so the marks
  have to outnumber the resets by exactly one, and the run has to stay
  bounded; and
- 40 generated defined C programs at each of five optimization levels - 200
  target runs per compiler - match native execution under both GCC 3.3.2 and
  GCC 16.2.

See [`difftest/README.md`](difftest/README.md) for generated-program and ISA
coverage details.

## Boot images

Hardware-style boot reproduces the mask ROM's externally visible effect:
copy the first 512 bytes of serial FLASH to RAM, set the internal-RAM stack,
and enter the MBR. All later stages execute their real firmware:

| Stage | Location |
| --- | --- |
| mask-ROM effect | emulator startup |
| MBR | serial FLASH offset `0x1` |
| menu | serial FLASH offset `0x300` |
| file-loader | serial FLASH offset `0x2300` |
| kernel and applications | FAT32 card |

Build the FLASH image from the repository root with:

```sh
make AWK=awk mbr
```

A card directory contains `kernel.elf`, `init.app`, `wiki.app`, fonts,
`wiki.inf`, and a language data directory such as `enquote/`. On macOS a
512 MiB image can be made with:

```sh
hdiutil create -size 512m -fs "MS-DOS FAT32" -volname WIKIREADER \
    -layout NONE -format UDRW -srcfolder /path/to/wikireader-card -o wrcard
mv wrcard.dmg emulator/images/wrcard.img
```

## Decoder regeneration

Regeneration is needed only when the ISA tables change and requires a C33
binutils installation. Disassemble all 65,536 words once as the all-core raw
binary and once in a PE ELF, place the listings in `allinsn_full.txt` and
`allinsn_pe.txt`, then run:

```sh
make tables
make test
make test-manual
```

`tools/derive_fields.py` solves operand fields; `tools/fit_ext.py` and
`tools/fit_data_ext.py` verify prefix composition. Generated tables are
committed and are generation-time artifacts, not runtime dependencies.

GMMan's Ghidra processor module can be used as an independent third decoder
oracle. Compile its `data/languages/s1c33.slaspec` with Ghidra's
`support/sleigh`, install the optional `pypcode` Python package, and run:

```sh
make test-sleigh S1C33_SLEIGH=/path/to/s1c33_sleigh
```

The check runs all 65,536 base words. It permits only the 370 cases where the
core manual says a reserved special-register, stack-register, or PSR-bit
operand executes as a no-op and SLEIGH elects not to decode it. SLEIGH p-code
is not used as an execution oracle: its upstream README still lists semantic
validation as unfinished, and the current source has known carry-in and
`popn` issues.

Do not add unused SoC peripherals solely for completeness.

## Layout

| Path | Purpose |
| --- | --- |
| `src/c33.[ch]` | CPU execution, traps, and interrupt entry |
| `src/mem.[ch]` | memory map and MMIO dispatch |
| `src/elf.c` | ELF32 loader |
| `src/sdcard.c`, `src/dma.c` | SPI SD card and DMA |
| `src/sdramc.c` | SDRAM controller and timing |
| `src/lcd.c`, `src/display.c`, `src/touch.c` | panel and input |
| `src/itc.c`, `src/cmu.c`, `src/timer.c` | interrupts, clocks, and timers |
| `src/port.c`, `src/periph.c`, `src/wdt.c` | GPIO, ADC, and watchdog |
| `c33_forms.h`, `c33_pe_valid.h` | generated decode data |
| `tools/` | generators and focused model tests |
| `difftest/` | native-versus-C33 execution tests |

## Known boundaries

- Illegal instructions in delay slots do not have an explicit unstable-state
  model.
- Cross-bank SDRAM command and data overlap is intentionally conservative,
  and the manual does not document enough to do better.
- Storage timing is fitted to one card; wake latency remains simplified. The
  modelled card restart of roughly 11.5 ms is sequencing and settling only,
  against 174 ms measured on a real card.
- Booting an application ELF directly skips the board's SDRAM initialization
  and leaves the memory system free. Boot `grifo.elf`, which programs the
  controller, or the FLASH chain.
- Stack-special and indirect jump forms that no firmware or generated C test
  retires have no independent runtime case.
- An emulator/firmware match alone is not proof of silicon behavior; the
  manual, binutils, focused model tests, and differential runs provide the
  independent checks above.

## UART0 console input

`--uart-input FILE` feeds bytes to the console UART through its four-byte RX
FIFO and interrupt controller. Use `-` for stdin. `--uart-start N` delays the
first byte until cycle N (default 1,000,000); `--uart-gap N` spaces bytes by
N cycles (default 50,000). File input is backpressured when the FIFO is full.
Newlines are passed unchanged. These options are independent of `-K`, which
types on the original firmware's touch keyboard.

For an NSH image, for example:

```sh
./wremu --uart-input commands.txt --uart-start 1000000 -n 100000000 nuttx
```

`make test-uart` checks FIFO ordering, overflow, receive/error interrupt
priority, flag reassertion while data remains, UART reset, and that transmit
takes the time the line takes.

Transmit is timed from the baud rate registers the firmware programs: a bit
is 2 * (BRTRD + 1) DIVMD clocks of the CPU's clock, a frame ten of them --
10,400 cycles at 57600 baud -- and the transmitter is the hardware's one-byte
buffer in front of a shift register, so TDBE clears while a byte waits and
firmware that polls it waits a frame a byte from the third byte of a run.
It was measured before it was modelled: the translator's first run on
silicon (`riscv/`) read 1.75x the emulator, and the whole difference was
console output, four hundred million cycles of a Linux boot, which this
charged nothing for.

Scripted `-T` taps, `-N` buttons and `-G` drags each accept up to 256 events.
Malformed events and scripts exceeding this limit are rejected instead of
silently dropping input. This accommodates full NuttX soft-keyboard commands.
