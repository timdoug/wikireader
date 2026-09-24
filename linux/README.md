# Native C33 Linux

This directory contains the no-MMU Linux port for the Epson S1C33E07
WikiReader. Linux executes directly on the C33. `wremu` models the WikiReader
hardware for development and regression testing; it does not run Linux under
a different guest ISA.

The current milestone boots the complete production-style path:

```text
mask-ROM behavior -> serial-FLASH MBR -> FAT32 file-loader
                  -> kernel.elf at 0x10040000 -> native C33 Linux
```

The same kernel is also emitted as `linux/artifacts/linux.app` for the normal
Grifo tiled launcher. In that path the card keeps Grifo as `kernel.elf`, and
the menu loads Linux at its existing `0x10040000` link address just as it loads
`nuttx.app`. Linux disables Grifo's application watchdog before bringing up
the kernel, saves the resident trap table, and hands `poweroff` and `reboot`
back through Grifo; reboot therefore returns to the launcher. The direct image
remains available as a recovery and bring-up path.

Whatever arguments the launcher's `init.ini` line carries become the kernel
command line. Grifo enters an application as `main(argc, argv)`, so the kernel
copies those strings out of the launcher's memory before anything can reuse it
and appends them to the built-in line; a later argument therefore wins over an
earlier one, and a line with no arguments still gets a console. `console=`,
`loglevel=`, `init=` and `earlycon=` are consequently editable on the card
without building anything. Grifo parses at most ten arguments totalling 256
bytes, and it is the only thing on this machine that can supply any: a direct
boot arrives with whatever the loader left in those registers, so the kernel
trusts them only when the incoming trap table says a launcher is resident.

Linux takes its memory size from the SDRAM controller's address
configuration rather than a build-time constant, so one image serves both the
16 MiB production boards and the 32 MiB early ones; the same probed limit
bounds the addresses the SPI driver will hand to HSDMA. It registers the
S1C33 interrupt controller with Linux's generic IRQ subsystem. The timer,
both UARTs, and SPI receive-DMA paths use normal `request_irq()` registrations
visible in `/proc/interrupts`. Linux runs the scheduler, registers the
interrupt-driven `ttyC0` UART console, and runs static BusyBox 1.38 as PID 1.
BusyBox init supervises an interactive Hush recovery shell on `ttyC0` and a
separate framebuffer console on a Unix98 PTY.

Time comes from the 16-bit timer block through a `drivers/clocksource` driver
rather than a jiffy tick. Channel 0 counts MCLK and its inverted comparison-B
output is wired on the board to channel 5's external clock input, so the pair
is one free-running 32-bit counter; that counter is the clocksource, the
scheduler clock, and the reference `udelay()` spins against, which makes delays
independent of where the linker placed the loop. Channel 2 is a clock event
with both periodic and one-shot modes, so the kernel runs with high-resolution
timers and an idle tick that stops. `loops_per_jiffy` is set from MCLK instead
of being measured, and a sleeping process now wakes when it asked to rather
than at the next tick.

Early userspace also runs a process-lifecycle regression. It
uses the asm-generic `clone(CLONE_VM | CLONE_VFORK)` ABI, executes a second
bFLT image as `/child`, and reaps its exit status with `wait4`. This
exercises the live syscall register frame, task creation, scheduling, exec,
exit, and parent wakeup without relying on a C library.

The same regression installs a `SIGUSR1` handler with `rt_sigaction`, delivers
the signal to PID 1, and returns through the C33 `rt_sigreturn` trampoline. The
kernel saves and restores the complete integer context, signal mask, and
alternate-stack state in an aligned `ucontext` frame on the userspace stack.

Traps reach the kernel through `CONFIG_GENERIC_ENTRY`, so tracing, seccomp,
and audit see every system call and the exit path is the generic one. The same
regression proves it: a child that calls `PTRACE_TRACEME` before `execve()`
must be stepped through several `PTRACE_SYSCALL` stops before it reaches its
exit status, instead of running straight there.

PID 1 then executes a static uClibc-ng bFLT program. The program enters through
the C33 CRT, calls `printf()` and `getpid()`, verifies `setjmp()`/`longjmp()`,
exits through libc, and is reaped by PID 1. This is the first regression using
the conventional C userspace ABI rather than the initramfs syscall veneers.

With a serial adapter attached, `earlycon=s1c33,mmio,0x300b00` reports through
the standard early console from the first parsed parameter until `ttyC0`
takes over and the boot console hands off.

The architecture's small early renderer writes `C33 LINUX` into the LCD
memory left active by the card loader, then becomes a temporary printk console
that scrolls 40 columns by 24 rows of the ordinary kernel log in Linux's
standard 6x8 font until late init. Boot checkpoints remain visible below the
text and an unhandled exception replaces their strip with a solid fault bar,
so failures before userspace remain visible without attaching to the serial
pads. The boot logo is placed on the physical right edge.
Once init is running, `/sbin/wr-console` takes over the ordinary fbdev device.
It renders a 40-column terminal and soft keyboard, allocates a Unix98 PTY from
`/dev/ptmx`, makes the PTY slave Hush's controlling terminal, and translates
released soft keys into terminal input. Its four-row keyboard provides
lowercase and shifted letters, `123`/`ABC` symbol pages, Control, Tab, Space,
cursor keys, Backspace, and Enter. The frontend writes only changed text rows
and key bands through fbdev. Packed glyph writes and overlap-safe framebuffer
row moves make scrolling cheap; bounded output frames are paced when they
scroll so commands remain visibly animated instead of either repainting every
byte or jumping directly to their final screen. Pacing never sits between a
key and the first thing that key produced: the first frame of a burst is
painted immediately and only its continuation is paced, which keeps the
touch-to-shell latency the boot test bounds. BusyBox init respawns the
frontend if it exits; the
independent `ttyC0` recovery shell remains available throughout.

Suspend-to-idle works: `wr.suspend=<seconds>` on the launcher's line makes the
console freeze the machine once nothing has touched it for that long, and the
next touch resumes it, and lights the panel itself on the way out: the kernel
spends that touch as its wake event, so no key arrives to do it and the
machine would otherwise resume to a dark screen indistinguishable from a dead
one. The interrupt that ends a suspend is taken as the wake event rather than
handled, so the architecture asks for software resend: without it the
character that woke the machine is never read, the receiver latches its
overrun and the panel is ignored from then on -- a machine that wakes once and
then answers nothing. The serial driver clears that state on resume as well.
The panel comes back before anything slower runs, since whatever the resume
path does next is time the screen spends dark; `wr.pmlog` asks the console to
append the interrupt tables either side of each suspend to `linuxpm.txt`, which
is the only account of a wake a device with no serial can give, and is off by
default because writing a card is slow enough to feel. A slow timer channel
stays armed across the freeze,
because suspend-to-idle stops the tick and then halts, which assumes the core
leaves HALT for whatever interrupt is meant to wake it; the HSDMA completion
cause demonstrably never woke it on silicon, so the core is brought back every
`s1c33_wake=<seconds>` to take whichever wake interrupt is already pending.
`s1c33_wake=0` removes the poll. With it removed the device still wakes on the
first touch, so this core does leave HALT for that cause and the poll is
insurance against the ones it might not, the way it ignores an HSDMA
completion; it is slow for the same reason. It is off unless that argument is given, because a
machine that suspends without a working wake source needs its batteries pulled.
UART1 carries the standard `wakeup-source` property, so the serial driver arms
its receiver as a wake interrupt instead of suspending the port, and the
interrupt controller advertises `IRQCHIP_SKIP_SET_WAKE` because nothing powers
it down. Deeper states are not offered: no `suspend_ops` is registered, since
those need the SDRAM parked in self-refresh by code running from internal RAM.

The board carries one 48 MHz resonator and no backup cell, so the RTC block in
the chip has neither a timebase nor standby power and this machine cannot keep
wall-clock time at all. Early userspace therefore sets the clock from the date
on the image it just booted, which makes everything the device writes stamped
no earlier than its own install instead of 1970, and says so plainly when the
card offers nothing better than the FAT floor.

Every one of the 256 traps has its own entry and stub, generated rather than
listed. The table used to name only the vectors the port happened to use, so
the first interrupt from a timer channel added later reported itself as vector
255 and panicked; an unexpected interrupt now says which one it was, and the
fault dumps the interrupt controller's flag and enable registers so the cause
is visible rather than inferred.

The panel is registered with the Linux input subsystem as a 240x208
absolute touchscreen at `/dev/input/event0`, reporting `ABS_X`, `ABS_Y`, and
`BTN_TOUCH`. Its UART1 transport is a second S1C33 serial-core port connected
to the touchscreen through Linux's tty-backed serdev layer. The input driver
therefore contains only the controller packet parser and evdev reporting; UART
registers, baud programming, buffering, and interrupts belong to the serial
driver. The legacy board description publishes UART1 and its touchscreen child
as a software-node firmware graph. Serdev enumerates that child, matches its
`compatible` property against the driver's normal firmware table, and binds it
through the driver core without a platform wrapper or forced attachment. The
node also carries the standard `current-speed` and touchscreen dimension
properties consumed by the driver. Keyboard geometry, labels, press state, and
character translation are entirely userspace policy: the touchscreen driver
does not know about keys or TTYs.
The framebuffer driver also owns the two controls that stop the panel: the
controller's power-save field and the display-enable line, which is an ordinary
GPIO descriptor taken from the same software-node graph as the SD slot's chip
select. `FBIOBLANK` therefore works, and because there is no VT to blank the
screen on a machine that runs from two AA cells, the userspace console does it:
it powers the panel down after `wr.blank=<seconds>` of no touch, wakes on the
next one without letting that touch type, and keeps the panel on for
`wr.blank=0`. The launcher's `init.ini` line carries that number, so the idle
timeout is a card edit.

The same 240x208 one-bit memory is registered with fbdev as `/dev/fb0` for
ordinary applications. The early renderer remains independent of fbdev so it
can still report failures before platform drivers have probed. The kernel's
standard monochrome Tux asset is enabled and briefly drawn when fbdev probes;
the userspace console replaces it after init completes.

The card ROM and MBR load `kernel.elf` with the S1C33E07 still running from
its 48 MHz OSC3 reset clock; the 60 MHz PLL setup normally belongs to Grifo,
which this boot path replaces. Linux decodes the live CMU clock selection and
publishes MCLK through the common clock framework, with the clock-management
unit's peripheral gates as its children. Both serial ports and the SPI
controller acquire and enable a standard gated clock, and the SPI driver takes
a second one for HSDMA; their drivers no longer call a board-specific clock
callback or receive a copied clock rate. Only the timer block is gated by
hand, because it starts before any provider exists. The
clocksource and clock event take their rate from the same hardware decoder,
which runs before clock providers exist.
This also keeps a kernel entered by already-running firmware correct if that
firmware selected the PLL first. UART initial speeds and the touchscreen
link's receive-only wiring are firmware properties, so the serial driver has
no private platform data or board-supplied register encodings.
The compact defconfig also enables the kernel's section garbage collection;
unused code and data from generic subsystems are discarded while C33 retains
its faster short-call model.

PID 1 is ordinary linked C apart from its entry point and syscall veneers.
The local ELF-to-bFLT converter carries plain `R_C33_32` pointers and C33's
split `R_C33_H`/`R_C33_M`/`R_C33_L` absolute addresses into the bFLT relocation
table. The kernel loader reconstructs and rewrites those three-instruction
addresses when it maps the process. The regression image deliberately contains
string pointers in text, initialized data, and BSS state.

## macOS and Linux responsibilities

The macOS host owns the checkout, Lima orchestration, `wremu`, fixture
generation, and artifact inspection. A small ARM64 Debian Lima VM builds the
C33 cross-toolchain and kernel because Linux Kbuild and the historical C33
toolchain are most reliable on a real Linux userspace.

The VM uses Apple Virtualization.framework on Apple Silicon. Its kernel source,
toolchain build trees, and kernel output tree stay on the VM's native disk;
only port sources, build recipes, and final artifacts cross the VirtioFS mount.
This avoids putting a Linux kernel build tree on macOS APFS or VirtioFS.

## One-time setup

From the repository root:

```sh
make -C emulator
make -C linux vm
make -C linux provision
make -C linux toolchain
```

`provision` installs the Debian build prerequisites. `toolchain` builds a
Linux-hosted `c33-epson-elf-` GCC/binutils toolchain; it does not reuse the
Mach-O executables under `host-tools/toolchain-c33/work`.

The boot fixture also links the existing MBR support libraries. Build them
with the repository's normal firmware targets if they are not present.

## Build and test

```sh
make -C linux fetch
make -C linux libc
make -C linux busybox
make -C linux console
make -C linux build
make -C linux boot-test
make -C linux app-test
```

`fetch` reconstructs the pinned upstream kernel and uClibc-ng revisions from
`revisions` on the VM disk, applies their patches, then installs their
overlays. Each kernel build also refreshes `overlay/` in the VM source tree so
iterative port changes cannot be silently missed. It leaves the final kernel
in `linux/artifacts/`, along with the stripped `linux.app` and its Tux launcher
icon. `app-test` boots the real Grifo menu in the emulator, taps that icon,
requires Linux and BusyBox to start, then uses the standard reboot syscall and
requires Grifo's watchdog reset to return to the menu.

`libc` builds and installs a static, no-MMU C33 uClibc-ng with the native
asm-generic syscall ABI and time64 interfaces. Its link regression compiles a
real `stdio.h` program, resolves it with the C33 PE `libgcc`, verifies that the
ELF has no undefined symbols, and converts it to a Linux-loadable bFLT image at
`linux/artifacts/uclibc-smoke`. The regular `build` target depends on this
image and embeds it in the initramfs as `/uclibc-smoke`.

`busybox` builds the pinned BusyBox 1.38.0 release as a static C33 bFLT. Its 79
enabled applets cover an interactive `hush`, core file and text tools,
checksums, archive/compression tools, filesystem inspection, and recovery
utilities. The regular `build` target installs it as `/init`, `/bin/busybox`,
and conventional applet symlinks. Its `rcS` runs the freestanding process,
signal, and libc diagnostics, then exercises Hush control flow and a
representative file/text/archive tool chain before mounting the SD card.
`init` finally respawns an interactive `hush` on `ttyC0`. `/diag-init`
remains available as the old freestanding rescue shell.

`console` builds the static bFLT framebuffer frontend. It uses only standard
fbdev, evdev, Unix98 PTY, devpts, process, and TTY interfaces; the application
contains no S1C33 register access or private kernel ABI. The regular `build`
target embeds it as `/sbin/wr-console`, and BusyBox init supervises it beside
the serial recovery shell.

The native S1C33 SPI controller driver and Linux's generic `mmc_spi` stack
power and pin-mux the WikiReader card slot, identify SDSC and SDHC cards, and
expose standard devices such as `/dev/mmcblk0p1`. The controller presents
ordinary 8-bit full-duplex SPI semantics, but batches bulk transfers into
32-bit hardware characters while preserving their wire byte order. It handles
all four SPI modes and the hardware's MCLK/4 through MCLK/512 divisors. It
reprograms the clock before chip select is asserted because disabling the
S1C33 serial block while it drives SCLK creates a real stray edge. The kernel
registers all 56 port lines through gpiolib; SPI core acquires the SD slot's
active-low chip select from the board's software-node graph and toggles its
GPIO descriptor. Board code declares the slot itself through
`spi_register_board_info()`, so the controller driver registers no devices of
its own. The card's 3.3 V rail and the level buffer between it and the S1C33
are two GPIO-switched fixed regulators that `mmc_spi` consumes as `vmmc` and
`vqmmc`; the settling time before the buffer may drive and the off time before
the rail may return are regulator constraints rather than sleeps in a board
callback.
It also sends aligned, all-ones bulk reads through the S1C33 HSDMA2/HSDMA3
transmit/receive pair. Short, unaligned, command, and write transfers retain a
bounded programmed-I/O path, so the optimization remains entirely behind the
standard SPI controller API. Probe establishes a documented reset-equivalent
state before requesting the HSDMA3 IRQ: it disconnects request sources, stops
both channels, clears their trigger and terminal-count latches, clears the SPI
DMA causes, selects standard mode, and programs the interrupt-priority byte
with every reserved bit zero. Bulk reads then sleep on a Linux completion
signaled by HSDMA3. A bounded latched-cause check closes the completion-timeout
race without changing the normal IRQ-driven path. Physical E07 parts stop
advancing SPI-triggered HSDMA if the otherwise-idle core executes
`HALT`, so the driver uses Linux's standard idle-poll control only while a DMA
transfer is active. The calling task still sleeps on its completion and other
runnable processes remain schedulable; only the idle task avoids `HALT` for
the duration of the transfer. The kernel includes FAT/VFAT and mounts the first
partition at `/mnt/sd` with synchronous writes. Early userspace leaves
`linux.ok` there as a persistent, serial-port-free boot report.

`boot-test` runs on macOS. It creates an isolated temporary FLASH/FAT32
fixture, boots it through the full emulated hardware path, and requires the
BusyBox PID 1 startup and one-shot diagnostic suite to complete without a
kernel panic. It then injects an `echo` command into the real `hush` over UART0
and verifies its output. It separately generates panel taps for a command and
Enter key, requires UART1 serial-core and serdev to deliver evdev records, and
requires the userspace frontend to execute that command through its PTY-backed
Hush. The test also checks the final display image for console text, all eleven
boot checkpoints, and the three keyboard rows. The fixture and emulator
display output are kept outside the checkout and removed afterward. The card
fixture is writable only for this isolated run; after the guest exits, the
host parses its raw FAT image and requires `linux.ok` to contain the expected
status. A console claim without persisted card bytes therefore fails the test.
The same regression requires the Linux driver to announce IRQ-driven HSDMA,
requires vector 25 to have a nonzero `/proc/interrupts` count, and requires the
emulator to report nonzero HSDMA2 transmit and HSDMA3 receive activity. It also
checks fbdev geometry, reads and rewrites the complete `/dev/fb0` image, and
requires the frontend to receive the scripted panel events from
`/dev/input/event0`.

## What comes next

The SPI clock-pin hold is the last board callback in platform data; it wants a
pin-control driver with a state that parks SCLK, which also means moving that
hold out of the interrupt-disabled window it lives in today. The embedded HSDMA
implementation still belongs behind DMAengine, which would also give the SPI
driver the DMA mapping API instead of a board-supplied address window.
Richer keyboard modes, console session management, and power management can
then grow around the proven LCD, touch, PTY, storage, and recovery userspace
paths.
