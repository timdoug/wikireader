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
`app-test.py` both pass, both through Grifo. It also runs X11 (Xfbdev, twm and
xeyes, by touch), so far only in wremu: see "X11: where it stands".

Device round trips now go through one script: `card/bin/check` is copied to
`bin/check` on the card, the user types `check` at the prompt, and it writes
`check.txt` (build, boot arguments, battery, the boot's card traffic, an
md5 of `linux.app` read uncached, a raw 4 MB card read untimed and again
timed by phase, Grifo's `bootlog.txt`, the kernel's warnings and the whole
timed kernel log). Keep it short; it was trimmed once already.

The card's `init.ini` line in use is:

```text
linux.ico : linux.app wr.blank=300 wr.suspend=600 wr.pmlog printk.time=1 s1c33_sd.timing=1
```

`printk.time=1` makes `check`'s kernel log a boot timeline (`wr-console`
ends it with "prompt up N s"), and `s1c33_sd.timing=1` counts card traffic
from boot. The card also holds `bootlog.on` (Grifo appends every
application load, with its phases, to `bootlog.txt`) and `zimlog.on` (ZIM
appends its startup to `zimboot.log`). All four are measurement switches:
take them off once the measuring is done, since each writes to the card or
the log.

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

A raw 4 MB read (`dd` from `/dev/mmcblk0`, page cache dropped) takes 2.87
to 2.89 s on the device (1.46 MB/s, 78% of the wire). The wire limit at
MCLK/4 is 1.875 MB/s, 2.2 s; the SPI block cannot divide MCLK by less than
4. Multiple-block reads are streamed (`sd_read_stream()`): the CPU finds the
first token, then the read comes in by DMA into a 64 KB ring while the CPU
finds each token and unpacks and checks each block behind it, by the
transfer's residue. Transfers restart as soon as they end and there is
room (`sd_stream_kick()` after each block, an hrtimer between requests).
The stream outlives the request: the stop command is answered without being
sent, and a request that reads on finds its blocks in hand (42 KB on
average). Anything else closes it (`sd_stream_close()`). While a transfer
runs the idle loop polls (`cpu_idle_poll_ctrl()`): HALT drops SPI DMA
requests for good. Per 512-byte block on the device (`read_timing`,
cycles):

| Phase | Cycles | What it is |
|---|---|---|
| token | 1,210 | first tokens (CPU, 4 of 37 requests) and the gaps |
| setup | 940 | looking at and restarting the ring's transfers |
| check | 9,760 | `sd_unpack_crc()`, ring to the request, from A0 RAM |
| poll | 1,090 | waiting for blocks to come in |
| outside the driver | ~8,000 | copy to user, page cache, the tick |

This card's gaps are 2 bytes, so a block is 517 bytes of stream, about
141 cycles a word against the wire's 128: 18,200 a block. A read takes
about 21,100.

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

## Boot: where it stands

On the device (launcher `init.ini` line with `printk.time=1
s1c33_sd.timing=1` and `bootlog.on` on the card, then `check`): Grifo loads
`linux.app` in 2.5 s (113 reads, 1.8 s of them reading), and the prompt is
up 4.12 s after the kernel starts. From the tap: the icon stays inverted
about 2.5 s, Tux about 4 s. Before this round it was 10 s and 7.4 s.

| Kernel uptime | Device | wremu (`SPC=1`, penalty 0) |
|---|---|---|
| SD host registered | 0.94 s | 0.94 s |
| `/init` (`rootstart`) | 1.18 s | 1.27 s |
| ext4 mounted | 2.13 s | 1.73 s |
| prompt | 4.12 s | 3.57 s |

Local sockets and VTs (for X) came after these measurements: about 0.6 s
more to the prompt in wremu, not yet timed on the device.

What this round found and did, device-only mostly:
- The device's 64 MB FAT32 boot volume has 512-byte clusters, so Grifo read
  every application one sector a command. FatFs `f_read` merges clusters
  that follow on the card; Grifo streams multiple-block reads
  (`grifo/src/sd_dma.c`). A transfer that ended 4 bytes short of the ring's
  end failed the stream on the device and left the rest to byte PIO (10 s);
  a one-word transfer now goes without the transmit DMA, in Grifo and in
  the Linux host, and a failed stream falls back to block-at-a-time DMA.
- The SD host's power-up waited for busy to end on a line that reads zeros
  until the card is in SPI mode: 3 s every boot. It now resets the card
  until it answers idle (at once, on this card).
- The root image's journal was replayed every boot (1.5 s): `late` freezes
  and thaws the root after the seed save.
- Linux side before that: no ext4 orphan file (0.34 s), `rootstart` mounts
  the kernel file systems, the self-tests in their own file, `seedrng` and
  the watchdog daemon from `inittab`, no KALLSYMS.

The device's boot traffic (read, then waiting for `late`): 1,871 blocks in
448 requests, 586 commands, 356 blocks written; writes are the dearest
phase (about 20,000 cycles a block read, overall). wremu's card answers a
command in 0.2 ms where the device's takes about 3, so wremu is optimistic
wherever a boot does small requests.

## Power states

No current has ever been measured in any of these; `zim/BATTERY.md` says so.

Grifo: running; ZIM's 20 ms HALT waits (clocks and SDRAM on, card on); deep
suspend whenever an application waits for input with nothing to do (CPU
clock to OSC3/32, SDRAM in self-refresh, peripheral clocks gated, `slp`;
the LCD controller keeps the image; the card's supply off with
`CARD_POWER=OFF`); and power off after 120 s in deep suspend, the rail cut.

Linux: idle HALT at full clock with SDRAM active (the tick stops when idle);
`wr.blank` powers down only the panel and its controller; `wr.suspend` (and
the power switch) is suspend-to-idle, processes frozen and the tick stopped,
still full clock and SDRAM active. There is no deeper state: no
`suspend_ops`, since parking SDRAM in self-refresh needs code in internal
RAM, as Grifo's `SuspendCode` does, and Linux cannot cut power. Grifo's
everyday idle is deeper than Linux's deepest state; that is likely the
largest battery gap in the system, more than any card policy.

`initcall_debug` durations in exact multiples of 10 ms are the CPU being
shared with other work until the next tick (PREEMPT_NONE), not idling: a
kthread's creation measured 5 to 85 ms busy with no idle entry. Boot is
CPU-bound; the idle column hardly moves before the console.

Left, largest first:
- **Restarting the card on every application start**: Grifo's phase log
  (`bootlog.txt`) puts 442 ms of `linux.app`'s 2.5 s in `File_open`, where
  wremu takes none. Grifo is built `CARD_POWER=OFF`, so a deep suspend (the
  launcher waiting for a tap) removes the card's supply and the next file
  operation initialises it again; this 128 GB card takes about 440 ms,
  `zim/BATTERY.md`'s 174. `CARD_POWER=KEEP` (a build switch in Grifo's
  Makefile) would keep it, at the card's standby current for at most the
  120 s before Grifo powers off; keeping it only while the launcher waits
  would need a syscall. The user is deciding; nothing measures current.
- **Initcalls**, about 0.9 s on the device: sysfs nodes, slab, message
  formatting, and power-up delays.
- **The boot's writes**: 356 blocks, the dearest card phase.
- **Grifo's reading**, 1.8 s for 2.9 MB. An LZ4 kernel was estimated and
  dropped: at 1.7 MB/s it saves about 0.1 s (decompression from A0 RAM at 8
  to 10 cycles a byte, against 35 a byte loaded).
- **`wr-console`'s start**, about 0.4 s to its first prompt, much of it the
  execs of the four processes init starts at once.

## X11: where it stands

Emulator only: **none of it has run on the device yet.** `startx` at the
console prompt brings up Xfbdev on the classic grey weave with twm and
xeyes; dragging on the background moves the pointer and the eyes follow,
holding still for 0.6 s opens twm's menu, and its Exit gives the panel back
to the console. `startx CLIENT -- SERVER-ARGS` works as usual.
`README.md` has the design; this is what exists and what it measured.

The pieces:
- Kernel: `CONFIG_NET` with only `AF_UNIX`; VTs behind the dummy console,
  registered on allocation (`patches/0018`); `FB_PROVIDE_GET_FB_UNMAPPED_AREA`
  for a no-MMU `mmap` of the panel; `INIT_STACK_NONE`; `__iramfunc` is
  `flatten`; the SD stream wait yields (`cond_resched()`).
- `initramfs/rootstart.c` makes `/dev/tty1..12`; `console/wr-console.c` holds
  VT 1 in `VT_PROCESS` mode and repaints on acquire.
- Buildroot: `buildroot/patches/0002` (X client libraries static) and `0003`
  (libXfont2 built-in fonts only); the defconfig adds X and
  `-ffunction-sections -fdata-sections`/`--gc-sections` for everything.
- `buildroot/external/package/xserver-kdrive`: xorg-server 1.19.7 Xfbdev with
  five patches (vfork, precompiled keymap, bit-reversed one-bit shadow,
  evdev absolute pointer and touch, clip-band bisection in `fbFillSpans`),
  `startx` and `wikireader.twmrc`.
- `buildroot/external/patches`: twm opens the root menu only after a still
  hold; xeyes asks for XInput 2.2, drops stale raw motion and moves its
  pupils without trigonometry; libX11 drops the East Asian charsets.
- The Linux toolchain's libgcc is soft-fp (`host-tools/toolchain-c33/gcc`,
  `c33/sfp-machine.h`); 2,000 IEEE vectors matched bit for bit
  (`artifacts/fptest`, gitignored).

Measured in wremu, launcher path:

| | |
|---|---|
| A drag frame (under twm) | 45 ms of CPU: X server 42%, kernel 38%, xeyes 15% |
| A frame's kernel work | about 57 syscalls (xeyes 37, mostly reads that find nothing), 14 context switches, 10 ticks; the scheduler's load tracking is a third of the kernel's share |
| `startx` to the first pupils | 6.3 s, most of it reading 3.3 MB of programs from the card; a second `startx` in the same boot loads the server in 0.6 s instead of 1.8 |
| RAM | 24.8 MB free at the console, 14.9 MB free (17.0 available) with X up; Xfbdev 4.0 MB, twm 1.6, xeyes 0.9 |
| Boot cost of sockets and VTs | about 0.6 s to the prompt |

Measured and rejected, so nobody repeats them: a `-Os` kernel (488 KB
smaller, 40% slower in the kernel), low-resolution timers (no gain), full
preemption (`ARCH_NO_PREEMPT` dropped: 215 KB more kernel, 0.9 s slower boot,
slower frames and a slower `startx`), `-extension RENDER` (the server starts
sooner but the clients are the critical path), starting twm before xeyes (no
difference), and dynamic linking (it would need FDPIC in the toolchain and
saves about 0.4 s; a BusyBox-style X binary would share as much).

The measuring tools are in `artifacts/perf/x11/` (gitignored), all through
Grifo: `xbench.sh LABEL` (eight scripted drags, frames and CPU per frame by
program), `xstart.sh LABEL` (the `startx` timeline from server-side probes),
`kcount.sh`/`ucount.sh` (kernel events and per-program syscalls per frame),
`uprof.py` (a `-F` profile attributed to the kernel and each X program),
`kvariant.sh NAME OPTIONS` (a kernel config variant in its own build
directory) and a copy of `timeline_app.py`. `APP=`, `IMG=`, `VMLINUX=`,
`XQ=` and `XEYES_BASE`/`TWM_BASE` choose what they run and how they read
it; the programs' load addresses move when their sizes do, so read them
from `/proc/PID/maps` after a size change. `ARGS='-- -nocursor'` measures
without the cursor (about a fifth of a frame).

## What is left, in the order I would take it

For X, before anything else: **run it on the device** (the card needs the
current `linux.app` and `linux.img`). One emulator run reset right after X
took the panel, with no watchdog timeout (a restart request); it did not
come back on rerun, but watch for it. Then, largest first:
- Stub the scheduler's load tracking on this single CPU: about 2.5 ms a
  frame and cheaper switches for every program, but it is a core scheduler
  patch; audit the 50-odd readers of the averages first.
- Read the X programs into the page cache in the background after boot:
  2-3 s off the first `startx`, for 3 s of card reading every boot.
- xeyes' Xt/xcb loop makes a `select`, a `poll` and several empty `recvmsg`
  a frame; one BusyBox-style binary for the X clients (0.6 MB less to read
  and hold); a smaller keymap; whether `-nocursor` should be the default.

The rest of the port:

1. **The CPU is the limit now**: token, check and the rest of the driver
   come to 13,000 a block and the work outside it to about 8,000, against
   the stream's 18,200, so no CPU saving can take a raw 4 MB read below
   about 2.5 s. The check is the largest part (9,760). Seven loads then
   seven stores in assembly, so that it changes SDRAM row twice per seven
   words instead of on every access, measured 9,700 on the device and no
   faster overall; its cost is the lookups and arithmetic, not the rows.
   The per-block bookkeeping (token 1,170, setup 940) is SDRAM-resident C
   at several cycles an instruction. The copy to user space is
   at the CPU's copy floor (about 4.0 cycles a byte), and the tick is 7% of
   the CPU when busy (HZ stays 100, the user's call).
2. **wremu's `dma_cpu_penalty`** (15) starves a stream left running beside
   a copy, which the device does not (see `README.md`); judge streaming
   changes with `WREMU_MODEL=dma_cpu_penalty=0`, or refit the penalty.
3. **PIO from A0 RAM** could beat the stream's 13 cycles a word, but it
   leaves DMAengine, which the user wants kept. Only if that changes.
4. **ITC priorities.** The controller's priority nibbles are still written
   by the drivers that know their cause (the timer, the serial ports, and
   the pin controller for the buttons); an `irq_set_priority`-style
   extension on the irqchip would move them.
5. **fbcon/VT.** `console/wr-console.c` is a userspace terminal. Its soft
   keyboard is a `uinput` device and it feeds every keyboard-shaped evdev
   node into the PTY, so keys reach any program; what remains is that the
   terminal itself is not the kernel's. The pacing, blanking, and suspend
   policy in it genuinely belong in userspace. The kernel has VTs (dummy
   console only), and `wr-console` holds VT 1 in process mode, so X and
   other full-screen programs take the panel from it and give it back.
6. **elf2flt** itself. `c33-linux-uclibc-ld` takes elf2flt's `-elf2flt`
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
  starts** (5.24 to 5.70 s for the same build): `/etc/init.d/late` starts
  it 20 s after boot, after `seedrng`, from the card. `check` waits for it.
- **`wremu` profile windows (`-y`) are in its clock's milliseconds**, which
  run about 3.5 s ahead of the guest's uptime on the launcher path.
- **wremu's `-T`/`-G`/`-N` times are instructions retired**, and the guest's
  clock runs about 1.3 times ahead of them, so a tap meant for 30 s lands
  near 39 s, possibly after the script has powered off. Keep the guest's
  shell busy well past every scripted input. `WREMU_DRAG_MS` is in
  milliseconds (60,000 cycles), not cycles.
- **Buildroot's `PKG-rebuild` recompiles but may not relink.** Xfbdev does
  not depend on `libkdrive.a` in automake, so an edited kdrive source came
  out in an old binary; use `PKG-dirclean`, and grep the target binary for a
  debug string before trusting a run with it.
- **X's scheduling slice is sized for fast machines.** With the default 5-15
  ms, one arc fill used up a client's slice and the idle handler put
  half-drawn frames on the panel: `startx` passes `-schedInterval 100
  -schedMax 100`.
- **An XInput 2.0 client gets no raw motion while another client holds the
  pointer grab** (twm, for any drag it received); 2.1 and later get it.
- **twm opens menus only from buttons**: `f.menu` bound to a key does
  nothing, silently. And `RR_Rotate_0` is 1, not 0.
- **wremu's `c33_handle_irq` probe counts syscalls too**: they enter the
  kernel the same way.
- **In hush, a redirection that fails comes before the ones after it**: put
  `2>/dev/null` before `< file`. And a shell function run inside `$(...)`
  (a re-exec on no-MMU) returned empty pattern expansions; `startx` uses
  builtins only.
- **A polling loop that calls `readl(host->base + ...)` reloads `host->base`
  from SDRAM each pass**, because `readl` clobbers memory. In the SD
  driver's wait loops a local copy made no measurable difference; in a
  hotter loop, take the address into a local first.
