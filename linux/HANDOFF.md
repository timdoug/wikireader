# C33 Linux hand-off

`README.md` describes what the port does and how to build it. This file is for
whoever picks the work up next: what has been proven and where, what is left,
and the things that cost a day to learn.

**NEVER BYPASS GRIFO.** Every emulator run, whether a test, a timing or a quick
check, boots MBR -> Grifo -> init.app -> linux.app, as `boot-test.sh` and
`app-test.py` do. A direct boot runs at 48 MHz without Grifo and is never
right.

## State

The port runs Linux 7.2.8 (`revisions`). It boots the production path and the
Grifo launcher path, mounts the card, runs BusyBox as PID 1, draws a
userspace terminal on the panel, takes touch input, blanks the display when
idle, suspends to idle, and wakes on a touch. Battery and board temperature
come through IIO, `generic-adc-battery` and `ntc_thermistor`; the watchdog
through the watchdog core; the pins through a pinctrl driver
(`drivers/pinctrl/pinctrl-s1c33.c`, which also owns the GPIOs); the SD card's
HSDMA through DMAengine (`drivers/dma/s1c33-hsdma.c`). `boot-test.sh` and
`app-test.py` both pass, both through Grifo.

Device round trips now go through one script: `card/bin/check` is copied to
`bin/check` on the card, the user types `check` at the prompt, and it writes
`check.txt` (build, boot arguments, battery, an md5 of `linux.app` read
uncached, a raw 4 MB card read untimed and again timed by phase, and the
kernel's warnings). Keep it short; it was trimmed once already.

The card's `init.ini` line in use is:

```text
linux.ico : linux.app wr.blank=300 wr.suspend=600 wr.pmlog
```

The console counts only input and screen output as activity, and `check`
writes to its file, so shorter timeouts blank the panel (which stops the LCD
controller's IVRAM reads and changes the bus under a timed read) or suspend
the machine mid-run.

Boot arguments reach the kernel from that line. Other knobs: `wr.pmlog` appends
`/proc/interrupts` either side of each suspend to `linuxpm.txt` on the card,
`s1c33_wake=<seconds>` sets the suspend wake poll (`0` disables it),
`no_console_suspend` keeps printk alive through the suspend path, and
`earlycon=s1c33,mmio,0x300b00` reports before platform drivers probe if a
serial adapter is attached. The console only prints warnings unless the line
carries `loglevel=7`, which has to come before `earlycon=` for the early
messages to show.

## What is proven on hardware, and what is not

A stock unit has no serial, so device results come back as files early
userspace writes to the card: `linuxhw.txt` (memory, clocksource, regulators,
input devices, contrast, the freestanding tests, interrupt counts, date) and
`linuxpm.txt` under `wr.pmlog`. `linuxhw.txt`, `linux.ok` and the userspace
checks behind them only run with `wr.selftest` on the card's `init.ini` line,
for example `linux.ico : linux.app wr.selftest`; a plain launcher boot skips
them to reach the console sooner. The report starts twenty quiet seconds after
the card mounts and takes another six, so give a boot half a minute before
pulling the card; the delay keeps its execs and sync writes off whatever is
being typed, which is also what makes the boot test's latency bound stable. The first silicon report is
checked in as `linux-device.txt`.

Validated on the user's board:

- SDRAM size probed from the controller; 32 MiB on that unit.
- `s1c33-t16` is the live clocksource, so timer 0's output really does clock
  timer 5 on the board, and the tick stops when idle.
- MCLK is 60 MHz under Grifo, against the 48 MHz reset clock.
- Display blanking, suspend-to-idle, and tap-to-wake.
- A touch wakes this core out of HALT by itself. Measured with the poll
  disabled (`s1c33_wake=0`): the timer-3 count stayed at zero across the sleep
  while the UART1 receive count climbed. The poll is slow insurance for causes
  this core ignores, such as an HSDMA completion, not the wake path.

Validated 2026-09-24 in a second round trip (`linux-device.txt` is that
report): the clock gates, `spi_register_board_info()`, the SD regulators and
`GENERIC_ENTRY`. On the board `sd-vcc` and `sd-buffer` both report enabled with
one user each, the card enumerates as `spi0.0`, the three freestanding tests
pass, and blank, suspend and tap-to-wake still work. The card was a 119 GiB
SDXC.

Two emulator gaps to keep in mind, since they are the reason that round trip
was needed:

- `wremu` honours the CMU gate bits for the timers, the watchdog, HSDMA and,
  since this round trip, the SPI block and both serial ports
  (`emulator/src/cmu.c`). An unclocked block drops writes, reads as zero and
  raises nothing; the run summary's `cmu gates` line counts the traffic. So a
  wrong gate mask now fails the boot test instead of waiting for silicon.
- The emulator models the SD rail (P32, active low) and the card refuses to
  work unpowered, which is why the regulator conversion failed loudly here
  before it could fail on the card. Since this round trip it also models the
  level-buffer enable (P33): with the buffer off the card sees no clock and
  no chip select and MISO idles high, and the `sd:` summary line counts the
  exchanges made in that state. A driver that talks before `vqmmc` is up now
  fails here.

Validated 2026-09-27 on the user's board: kernel 7.2.8; battery (2.66 V,
the same reading Grifo's own formula gives) and temperature (about 24 C);
the watchdog, with its daemon running; the pin registers matching what the
old board code set, and the buttons and power switch interrupting; SD reads
through DMAengine, byte for byte, at every step of the read work below.
Grifo leaves P63 as #WDT_NMI, which is harmless with NMI off.

## SD card reads: where they stand

A raw 4 MB read (`dd` from `/dev/mmcblk0`, page cache dropped) takes 3.14
to 3.20 s on the device (1.3 MB/s, 70% of the wire). The wire limit at
MCLK/4 is 1.875 MB/s, 2.2 s; the SPI block cannot divide MCLK by less than
4. Multiple-block reads are streamed (`sd_read_stream()`): the CPU finds the
first token, then the read comes in by DMA as long transfers into a 64 KB
buffer while the CPU finds each token and unpacks and checks each block
behind it, by the transfer's residue. The stream outlives the request: the
stop command is answered without being sent, the transfer is left the
whole buffer (`sd_stream_prefetch()`), and a request that reads on finds
its blocks coming in (41 KB in hand on average). Anything else closes it
(`sd_stream_close()`). While a transfer runs the idle loop polls
(`cpu_idle_poll_ctrl()`): HALT drops SPI DMA requests for good. Per
512-byte block on the device (`read_timing`, cycles):

| Phase | Cycles | What it is |
|---|---|---|
| token | 1,110 | first tokens (CPU, 4 of 37 requests) and the gaps |
| setup | 110 | starting the stream's transfers |
| check | 9,290 | `sd_unpack_crc()`, stream buffer to the request, from A0 RAM |
| poll | 4,810 | waiting for blocks to come in |
| outside the driver | ~7,000 | copy to user, page cache, the tick |

This card's gaps are 2 bytes, so a block is 517 bytes of stream, about
141 cycles a word against the wire's 128.

Read a block at a time (single blocks, and cards that shift their tokens,
which also stop streaming for good), a block's transfer takes about 19,600
cycles against 16,400 on the wire whatever the CPU does beside it. Measured
on the device and ruled out as the cause: writing to IVRAM bounce buffers
instead of SDRAM (20,700), eight loads then eight stores in the check
(20,400 against 20,300 to 20,500), the check from its SDRAM copy (19,300
for the check alone; A0 RAM code does not crowd the DMA), and the check
after the transfer instead of beside it (19,600). Since a stream's words
cost 13 over the wire, most of the extra is each transfer getting going.

The user keeps DMAengine and the block CRC; both were asked and settled.

What was built on the way, in `README.md` in detail: the streamed read,
kept open across sequential requests, with residue and a fixed-address
(interleaved) transmit descriptor in the DMA provider; the pipelined block-at-a-time read (start N+1, check N,
prepare N+2, wait); `sd_unpack_crc()`, slice-by-4 on
wire-order words plus the byte swap in one pass; A0 RAM code
(`asm/iram.h`, `kernel/iram.c`), where it, `memcpy`/`memset`/`memmove`
(`lib/string.S`, eight loads then eight stores) and division (`lib/div.S`)
run; the all-ones transmit buffer in IVRAM; `/sys/module/iram_bench/
parameters/run`, which times the same loops from SDRAM and from A0 RAM;
and `read_timing`. Emulator and device results of both tools are in
`artifacts/perf/` (gitignored).

wremu was changed to match (`emulator/README.md`, `dma_async`): SPI DMA no
longer freezes the CPU, and CPU data accesses to SDRAM hold its writes back.
Its driver total is now within 1% of the device's. It still cannot say
what the device's arbitration bound is (a larger penalty starves the write
queue into receive overruns, which the device never has), and its per-word
DMA cost is a fitted constant (`dma_extra`), so it cannot judge changes to
what the DMA does per word; those need the device.

## What is left, in the order I would take it

1. **The overlap is partial.** A raw read takes about 23,200 cycles a
   block against the stream's 18,200, and the CPU's own work (token, check,
   and about 7,000 outside the driver) comes to about 17,400. Either could
   set the pace if the other kept up. `read_timing` covers only the driver;
   what the stream does between requests (how long it runs, whether the
   copy slows it, whether 64 KB is ever full) is the next thing to count.
   The check reads the stream buffer and writes the request, two rows,
   alternately (9,300 against 8,300 to 9,100 in place); eight loads then
   eight stores might pay here where they did not beside the per-block DMA.
   The copy to user space is at the CPU's copy floor (about 4.0 cycles a
   byte), and the tick is 7% of the CPU when busy (HZ stays 100, the user's
   call).
2. **wremu's `dma_cpu_penalty`** (15) starves a stream left running beside
   a copy, which the device does not (see `README.md`); judge streaming
   changes with `WREMU_MODEL=dma_cpu_penalty=0`, or refit the penalty.
3. **PIO from A0 RAM** could beat the stream's 13 cycles a word, but it
   leaves DMAengine, which the user wants kept. Only if that changes.
4. **Each kthread creation costs about 10 ms**, unexplained.
5. **ITC priorities.** The controller's priority nibbles are still written
   by the drivers that know their cause (the timer, the serial ports, and
   the pin controller for the buttons); an `irq_set_priority`-style
   extension on the irqchip would move them.
6. **fbcon/VT.** `console/wr-console.c` is a userspace terminal. Its soft
   keyboard is a `uinput` device and it feeds every keyboard-shaped evdev
   node into the PTY, so keys reach any program; what remains is that the
   terminal itself is not the kernel's. The pacing, blanking, and suspend
   policy in it genuinely belong in userspace.
7. **elf2flt** itself. `c33-linux-uclibc-ld` takes elf2flt's `-elf2flt`
   options, but the conversion behind them is the local `make-flat.py`.
   Separately, **the overlay as a real patch series**, which only bites when
   the pinned stable tag is bumped.

Done since the second round trip, emulator-tested and **not yet run on
hardware**: the contrast PWM (`drivers/pwm/pwm-s1c33.c`, timer 1, with its
CMU gate now a clock the driver holds), the lcd-class consumer
(`/sys/class/lcd/wikireader/contrast`), the buttons and power switch as
polled GPIO keys, and the uinput keyboard path. The device round trip for
these should show, in `linuxhw.txt`, `contrast:2048` and three input device
names, and on the panel: a readable screen through boot (the gate stayed on
and the PWM was adopted, not restarted), `echo 3000 > /sys/class/lcd/wikireader/contrast`
darkening it, typing still working, the history button recalling the last
command, and the power switch putting it to sleep. The third round trip
(2026-09-25) proved the contrast (Grifo's saved 2216 came through), typing
through uinput, and the switch, after a lesson: P03 idles low and is
pressed high, the firmware's interrupt polarity comment notwithstanding,
and described active-low it re-pressed itself after every resume. The
`wr.pmlog` file now records why each suspend happened and what the P0 and
P6 port bytes read, which is how that was found without serial. Still
unproven on silicon: the buttons on P60..P62 read high when pressed.

Since then: the buttons and the switch interrupt instead of being polled,
device-proven 2026-09-27. The pin controller chains key input 0 (P60..P62, a
mismatch comparator it re-arms with each state it reads) and port input 3
(P03, one edge at a time, turned round after each), and they are an ordinary
`gpio-keys` device. Idle at the prompt fell from 4.4% of the CPU to 1.0%.
On the device, check that each front button and the switch still act once
per press; `/proc/interrupts` counts them on the `s1c33-gpio` lines.

Deliberately not framework code, because no framework equivalent exists: the
suspend wake poll and the clock seeding both work around the absence of an RTC
(the schematic has one 48 MHz resonator and no backup cell, so the SoC's RTC
block has neither timebase nor standby power), and the `wr.*` console knobs are
userspace policy.

Test debt: `app-test.py` choreographs scripted typing against instruction
counts and has drifted once already. `emulator` `make test-zim-copy` fails:
it boots the file loader as a direct ELF, which wremu refuses.

## Traps

- **`boot-test.sh` prints a filtered view of `boot.log`, not the log.** A new
  assertion has to be added in two places: the check list and the final display
  `grep -E`. It also once had two variables named `clock_expected`, which
  silently killed one assertion.
- **Always confirm an edit applied.** A scripted replace that did not match
  whitespace once cost a device round trip debugging a gate bit that was never
  written.
- **Check the schematic before assuming a block is usable.** The SoC has an RTC;
  this board cannot use it.
- **`reg-fixed-voltage` asks its firmware node for an under-voltage IRQ**, and a
  software node answers `-ENXIO`, which the driver treats as fatal. Use a
  `gpiod_lookup_table` for regulator enable lines.
- **Non-DT regulator lookups return `-ENODEV`, not `-EPROBE_DEFER`.** Ordering
  has no slack: the pin controller registers at `postcore_initcall` so the chip exists
  when the fixed regulators bind at subsys level, and the regulator devices are
  registered before the SD host.
- **`GENERIC_ENTRY` expects things from the arch that have no defaults:**
  `_TIF_UPROBE`, `PTRACE_SYSEMU`/`PTRACE_SYSEMU_SINGLESTEP`, `on_thread_stack()`,
  `regs_irqs_disabled()`, `arch_syscall_is_vdso_sigreturn()`, a
  `syscall_work` field in `struct thread_info`, and `HAVE_SYSCALL_TRACEPOINTS`.
- **`GENERIC_ENTRY` does not give you strace.** It makes `PTRACE_SYSCALL` work,
  which the early-userspace regression now proves, but strace itself has never
  been ported to this architecture.
- **Clocks the core needs early must not be registered as clocks.** The timer
  gates are set through a raw accessor on purpose: nothing would hold a
  reference, and the clock core turns off every gate no driver has claimed.
- **This BusyBox has no `stat -c` and no `tail -1`.** `date -s @epoch` and
  `date -r FILE +%s` do work.
- **The C33 backend is a V850 fork, and V850's `r0` is always zero.** Here
  `%r0` is a callee-saved register. The backend printed a numeric address as
  `[%r0+N]`, and took C++ enumerators for data; both are fixed, but check for
  that assumption when anything V850-derived misbehaves.
- **libstdc++ finds backend bugs that C never reached**: C++ enumerators with
  attributes, sibling calls through half of a 64-bit value, and numeric
  addresses on paths GCC proved dead. Build it with `make -k` to see every
  failing file at once.
- **Driving the guest's own shell over UART** answers questions about userspace
  in well under a minute, against a two-minute rebuild:
  `artifacts/perf/ask-app.sh 'commands'` boots through Grifo, types them,
  and powers off (gitignored; it wraps `/tmp/sepdata/timeline_app.py`).
- **Grifo's ELF loader loads segments by virtual address.** A section
  linked at 0x0c00 with its load address in the image came up there, and
  the kernel's own copy from the (empty) load address then zeroed it. So
  `__iramfunc` code is linked in `.text` and copied at boot, and must be
  position-independent: no calls, no data by address.
- **Keep checks of new assembly cheap.** Mirror the loop in Python and test
  it on the host (millions of cases in seconds), then boot once with a small
  table of precomputed answers. A C reference loop in the guest ran for
  minutes.
- **Untimed card reads on the device are noisy before the watchdog daemon
  starts** (5.24 to 5.70 s for the same build): rcS starts it 20 s after
  boot, with `seedrng`, from the card. `check` waits for it.
- **`wremu` profile windows (`-y`) are in its clock's milliseconds**, which
  run about 3.5 s ahead of the guest's uptime on the launcher path.
- **A polling loop that calls `readl(host->base + ...)` reloads `host->base`
  from SDRAM each pass**, because `readl` clobbers memory. In the SD
  driver's wait loops a local copy made no measurable difference; in a
  hotter loop, take the address into a local first.
