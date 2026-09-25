# C33 Linux hand-off

`README.md` describes what the port does and how to build it. This file is for
whoever picks the work up next: what has been proven and where, what is left,
and the things that cost a day to learn.

## State

The port boots the production path and the Grifo launcher path, mounts the
card, runs BusyBox as PID 1, draws a userspace terminal on the panel, takes
touch input, blanks the display when idle, suspends to idle, and wakes on a
touch. `boot-test.sh` and `app-test.py` both pass.

The card's `init.ini` line in use is:

```text
linux.ico : linux.app wr.blank=30 wr.suspend=60
```

Boot arguments reach the kernel from that line. Other knobs: `wr.pmlog` appends
`/proc/interrupts` either side of each suspend to `linuxpm.txt` on the card,
`s1c33_wake=<seconds>` sets the suspend wake poll (`0` disables it),
`no_console_suspend` keeps printk alive through the suspend path, and
`earlycon=s1c33,mmio,0x300b00` reports before platform drivers probe if a
serial adapter is attached.

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
- MCLK is 60 MHz under Grifo, against the direct-boot fixture's 48 MHz.
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

## What is left, in the order I would take it

1. **pinctrl.** `wr_spi_hold_clock()` in `arch/c33/kernel/devices.c` is the last
   board callback in platform data. It is not just a matter of writing the
   driver: the hold runs inside `local_irq_save()` in `s1c33_spi_configure()`,
   and `pinctrl_select_state()` takes mutexes and can sleep, so that critical
   section has to be restructured first — and it exists precisely because
   disabling the serial block while it drives SCLK puts a stray edge on the
   wire that can eat a card response bit. Doing it properly also means folding
   `gpio-s1c33` into a combined pinctrl+gpio driver, since one driver has to own
   the port registers.
2. **DMAengine.** HSDMA2/3 live inside the SPI driver, `NO_DMA` is selected so
   there is no DMA API at all, and the addressable window is passed as
   `dma_memory_start`/`dma_memory_end` in platform data instead of coming from
   `dma_map_single()`.
3. **Interrupt-driven buttons.** The front buttons and the power switch are a
   polled `gpio-keys-polled` device at 50 ms while the console has it open;
   the port block's KINT0 comparator could raise them instead once
   `gpio-s1c33` grows an irqchip half. The ITC itself is an irqchip behind an
   irqdomain now (`drivers/irqchip/irq-s1c33.c`), so that half has a parent
   to chain to.
4. **ITC priorities.** The controller's priority nibbles are still written
   by the drivers that know their cause (the timer and the serial ports); an
   `irq_set_priority`-style extension on the irqchip would move them.
5. **fbcon/VT.** `console/wr-console.c` is a userspace terminal. Its soft
   keyboard is a `uinput` device now and it feeds every keyboard-shaped evdev
   node into the PTY, so keys reach any program; what remains is that the
   terminal itself is not the kernel's. The pacing, blanking, and suspend
   policy in it genuinely belong in userspace.
6. **elf2flt** instead of the local `make-flat.py`, and **the overlay as a real
   patch series** — both only bite when the pinned stable tag is bumped.

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

Deliberately not framework code, because no framework equivalent exists: the
suspend wake poll and the clock seeding both work around the absence of an RTC
(the schematic has one 48 MHz resonator and no backup cell, so the SoC's RTC
block has neither timebase nor standby power), and the `wr.*` console knobs are
userspace policy.

Test debt: `app-test.py` choreographs scripted typing against instruction
counts and has drifted once already.

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
  has no slack: `gpio-s1c33` registers at `postcore_initcall` so the chip exists
  when the fixed regulators bind at subsys level, and the regulator devices are
  registered before the SPI controller.
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
- **Driving the guest's own shell over UART** answers questions about userspace
  in well under a minute, against a two-minute rebuild.
