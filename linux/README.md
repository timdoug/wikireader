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
16 MiB production boards and the 32 MiB early ones. The S1C33 interrupt
controller is `drivers/irqchip/irq-s1c33.c`, an irqchip behind a linear
irqdomain whose hardware interrupt numbers are the trap vectors; the arch
calls its init from `init_IRQ()`, the board file maps the vectors it puts in
platform resources through the domain, and the trap entry routes each vector
to the domain rather than treating it as a Linux IRQ number. The domain's
allocator prefers the hardware number when it is free, so `/proc/interrupts`
still reads in vectors. The timer and both UARTs use normal `request_irq()`
registrations visible there. Linux runs the scheduler, registers the
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
standard 6x8 font until late init. An unhandled exception draws a solid
fault bar below the text, so failures before userspace remain visible without
attaching to the serial pads. The boot logo is placed on the physical right edge.
Once init is running, `/sbin/wr-console` takes over the ordinary fbdev device.
It renders a 40-column terminal and soft keyboard, allocates a Unix98 PTY from
`/dev/ptmx`, and makes the PTY slave Hush's controlling terminal. Its four-row
keyboard provides lowercase and shifted letters, `123`/`ABC` symbol pages,
Control, Tab, Space, cursor keys, Backspace, and Enter. A released soft key is
not written to the PTY directly: the frontend registers the soft keyboard as
a `uinput` device and reports the key on it, with `KEY_LEFTSHIFT` and
`KEY_LEFTCTRL` around it as a physical keyboard would, so the key is visible
to any program that reads evdev. The frontend is also the terminal's
keyboard driver, since no VT carries a terminal: it opens every evdev node that has keys
and no absolute axes -- the soft keyboard, the front buttons, anything added
later -- and turns their presses into bytes for the PTY with a US keymap,
falling back to writing the PTY directly if `/dev/uinput` is missing. The
front buttons have no terminal meaning of their own and get the three things
a terminal without other keys most wants: random is an interrupt (`^C`),
search is Tab, and history is the up arrow. The power switch cannot cut power
from Linux, so it is the sleep button: it suspends until the next touch when
`wr.suspend` is set and blanks the panel otherwise. Only a press counts as
activity, so the release of the key that blanked the machine does not wake
it. The frontend writes only changed text rows
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
absolute touchscreen, found by its name rather than by event number since the
buttons register first, reporting `ABS_X`, `ABS_Y`, and `BTN_TOUCH`. Its UART1 transport is a second S1C33 serial-core port connected
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

The three front buttons on P60..P62 and the power switch on P03, all of
them pressed high, are a `gpio-keys` device described by software nodes,
reporting `KEY_F1` (random), `KEY_SEARCH`, `KEY_BACK` (history) and
`KEY_POWER`. They interrupt through the port block: key input 0 compares
P60..P62 with a stored pattern, and port input 3 watches P03 for the edge
opposite its level. The switch's polarity came from the board: described as
active-low, every resume reported a fresh press, because a suspend releases
all keys and the next report re-reads a pin that was never released, and
the machine slept again four seconds after each wake.

Pins belong to `drivers/pinctrl/pinctrl-s1c33.c`, a pin controller for ports
0 to 9 that is also the GPIO chip for ports 0 to 6. Every pin is a group of
its own with the manual's functions for it, so the board's pin map, a
`pinctrl_map` table, reads like the manual: P65 is `sdi`, P11 `tm1`, P70
`ain0`. The driver core applies each device's default state before its
probe, and requesting a line as a GPIO selects its port function, so the
board file writes no port registers. The SPI flash's chip select, which
shares the card's bus, is held high by a GPIO hog on the chip's software
node. The generic `output-low` configuration parks a pin as a port output,
which is how the SD host holds its clock still.

The panel's contrast is a PWM: the firmware runs timer 1 at MCLK/4096 with
comparison A as a 12-bit contrast number, 0 lightest and 4095 darkest, and
leaves it running. `drivers/pwm/pwm-s1c33.c` drives one timer channel as a
PWM chip, adopting the running configuration rather than restarting it and
moving only comparison A when the period is unchanged so the output keeps its
phase; it never touches the ADVMODE and PAUSE registers it shares with the
clocksource. `drivers/video/backlight/wikireader_lcd.c` consumes it through a
board `pwm_lookup` and exposes the firmware's number as
`/sys/class/lcd/wikireader/contrast`, reading the value back from the PWM at
probe so a boot changes nothing on the panel. Timer 1's CMU gate is a clock
the PWM driver holds, which is what keeps the panel lit once the clock core
turns off every gate nobody claimed.

The battery and the board temperature come from the chip's 10-bit A/D
converter, `drivers/iio/adc/s1c33-adc.c`, an IIO device that converts one
channel per read against AVDD, a fixed 3.3 V regulator it takes as `vref`.
Everything above it is a stock driver wired by software-node `io-channels`
references. AIN0 sees the two AAA cells through a 150k/1M divider, which
`iio-rescale` undoes as a `voltage-divider`, and `generic-adc-battery`
publishes the result as `/sys/class/power_supply/generic-adc-battery`, with
`voltage_now` in microvolts and `status` Discharging. AIN1 is a 100k NTC
thermistor under a 120k pull-up to the same rail; `ntc_thermistor` reads it
as `/sys/class/hwmon/hwmon*/temp1_input` in millidegrees. The part, a
TCT6GJ104H410, is not in that driver's tables, so the board names the Murata
NCP03WF104, another 100k thermistor with B = 4250 K. AIN2, the panel's V4
bias, is a plain IIO channel. Four upstream fixes make this work without a
device tree: `iio-rescale` and `ntc_thermistor` fall back to their
platform-device ID when there is no match data, `generic-adc-battery` treats
a battery nothing supplies as discharging rather than charging, and a
software node shared by a device and its same-named power supply no longer
warns about the duplicate sysfs link.

The chip's watchdog is `drivers/watchdog/s1c33_wdt.c` on the watchdog core:
a 30-bit counter on MCLK that resets the chip at most 17.9 s after its last
ping at 60 MHz, with the core pinging on the hardware's behalf for longer
timeouts. Grifo arms the watchdog before it starts an application, so the
kernel stops it at entry; the driver starts it again at probe and the core
feeds it until userspace opens `/dev/watchdog`, so a kernel that hangs while
booting is reset. BusyBox's `watchdog` daemon opens it 20 s after boot,
after the random-seed save, from `/etc/init.d/late`, which `init` starts
after the consoles since an exec during boot costs the console time: it
pings every 30 s with a 60 s timeout, so a stuck userspace resets
the device after a minute and a stuck kernel after 18 s, and a clean
shutdown stops it with the magic close. Suspend-to-idle is a halt, during which the counter
keeps running, so the driver stops it across a suspend. Only the reset output
is used; the NMI output, `#WDT_NMI`, is P63, which is wired to the power
logic. The watchdog is also the restart handler, so `reboot` resets the chip
when there is no launcher to return to.
The framebuffer driver also owns the two controls that stop the panel: the
controller's power-save field and the display-enable line, which is an ordinary
GPIO descriptor taken from the same software-node graph as the SD slot's chip
select. `FBIOBLANK` therefore works, and because no kernel console blanks the
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
unit's peripheral gates as its children. Both serial ports and the SD host
acquire and enable a standard gated clock, and so does the HSDMA controller;
their drivers no longer call a board-specific clock
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

The kernel's only networking is local (`AF_UNIX`) sockets, which X clients
use to reach their server. `CONFIG_NET` brings about 360 KB of networking
core with them, which no option removes: 0.4 s of every boot in wremu, 0.27 s
of it Grifo loading the larger image. The kernel also has virtual terminals,
with only the dummy console behind them: they draw nothing, and kernel
messages stay on the serial port. They decide which full-screen program owns
the display, as on a desktop. Registering all 63 VT devices at boot took
0.45 s, about 7 ms a device in sysfs and devtmpfs, so `patches/0018`
registers a VT's device when the VT is allocated, with one character device
for all of them; `rootstart` makes `/dev/tty1` to `/dev/tty12`, so a VT opens
by number before it exists. VTs cost about 0.2 s of boot.

PID 1 is ordinary linked C apart from its entry point and syscall veneers.
The local ELF-to-bFLT converter carries plain `R_C33_32` pointers and C33's
split `R_C33_H`/`R_C33_M`/`R_C33_L` absolute addresses into the bFLT relocation
table. The kernel loader reconstructs and rewrites those three-instruction
addresses when it maps the process. The regression image deliberately contains
string pointers in text, initialized data, and BSS state.

uClibc, BusyBox and the console are built `-msep-data` (see
`host-tools/toolchain-c33/gcc/ABI.md`). Their text holds no absolute address.
Each process reaches its own data segment through `%r15`, which
`start_thread` loads from `mm->start_data`, and every other address comes
from a relocated word in that segment. With no text relocation, the converter
leaves `FLAT_FLAG_RAM` clear. `binfmt_flat` then maps the text read-only from
the file, and the kernel shares that mapping between every process running
the program. The system is on ext4, which cannot map files directly, so the
first exec of a program copies its text into RAM once; every later process
running it shares that copy, because no-MMU Linux lets read-only private
mappings of one file overlay each other.
`make-flat.py --shared-text` fails the build if a text relocation appears.
The BusyBox suite checks that PID 1 and a child map the same `/bin/busybox`
text. The freestanding diagnostics still carry text relocations and still
load as private copies.

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
make -C linux fetch
make -C linux toolchain
```

`provision` installs the Debian build prerequisites. `toolchain` builds two
Linux-hosted GCC/binutils toolchains into one prefix; it does not reuse the
Mach-O executables under `host-tools/toolchain-c33/work`:

- `c33-epson-elf-` is the bare-metal compiler, used for the kernel.
- `c33-linux-uclibc-` is the userspace compiler, for C and C++. It defaults to
  `-mc33pe -msep-data -mlong-calls`, defines `__uClinux__`, and links
  statically against uClibc-ng in its sysroot. Its `ld` follows uClinux's
  elf2flt convention: `-Wl,-elf2flt` writes a bFLT and keeps the ELF beside
  it as `.gdb`, `-Wl,-elf2flt=-s<bytes>` sets the stack size, and
  `-Wl,-elf2flt=--shared-text` fails the link if the text needs relocation.
  Links without `-elf2flt` stay ELF but keep their relocations, so
  `initramfs/make-flat.py` can convert them later.

The userspace compiler is built against the kernel's UAPI headers and
uClibc-ng's headers, so `fetch` comes first.

The tests boot Grifo, `init.app` and the MBR flash from `samo-lib`. Build them
with the repository's normal firmware targets if they are not present.

## Build and test

**NEVER BYPASS GRIFO.** Every emulator run, a test, a timing or a one-off
check, boots the way the device does: MBR, Grifo, `init.app`, then
`linux.app` from `init.ini`. Both tests below do; the direct paths run at the
48 MHz reset clock without Grifo's hardware setup and test a machine that
does not exist.

```sh
make -C linux libc
make -C linux rootfs
make -C linux build
make -C linux boot-test
make -C linux app-test
```

`fetch` reconstructs the pinned upstream kernel, uClibc-ng and Buildroot
revisions from `revisions` on the VM disk, applies their patches, then
installs their overlays. Each kernel build also refreshes `overlay/` in the VM source tree so
iterative port changes cannot be silently missed. It leaves the final kernel
in `linux/artifacts/`, along with the stripped `linux.app` and its Tux launcher
icon. `app-test` boots the real Grifo menu in the emulator, taps that icon,
requires Linux and BusyBox to start, then uses the standard reboot syscall and
requires Grifo's watchdog reset to return to the menu.

`libc` builds a static, no-MMU C33 uClibc-ng with the native asm-generic
syscall ABI and time64 interfaces, and installs it with the kernel headers
into the `c33-linux-uclibc-` sysroot. Its link regression compiles a
real `stdio.h` program, resolves it with the C33 PE `libgcc`, verifies that the
ELF has no undefined symbols, and converts it to a Linux-loadable bFLT image.
Every program start sets a stack-protector guard from the clock
(`SSP_QUICK_CANARY`). Without an MMU the guard only catches bugs, and the
default of reading `/dev/urandom` would make the first program at boot wait
3.4 s for the kernel's random pool.

The library includes POSIX threads through LinuxThreads, the uClibc
implementation that works without an MMU and without thread-local storage.
The C33 port supplies `clone.S` and a `testandset` that masks interrupts for
its load and store. That is atomic on a single core, and the C33 has no
privilege level that would stop user code masking them. In a static uClibc
the thread library is part of `libc.a` and every program carries it: about
22 KB more shared text, and about 4 KB more private data per process once
the port caps threads and thread-specific keys at their POSIX minimums (64
and 128) instead of 1024 each. `libc` also builds
`linux/artifacts/pthread-test`. `app-test` runs it from the SD card: a
contended mutex, per-thread `errno`, condition variables, semaphores,
`pthread_once` and thread-specific data.

uClibc-ng has wide characters, which libstdc++ and many packages need; they
put about 8 KB of shared text into BusyBox. `libc` then builds libstdc++
against the installed C library, with `toolchain.sh libstdc++`.

C++ exceptions unwind with the DWARF tables, which cost nothing until
something throws. The tables hold absolute addresses, so they live in the
data segment, where the loader relocates them, and `crtbegin.o` registers
`.eh_frame` with libgcc's unwinder, as a bFLT has no program headers to find
it by. `.eh_frame` is an output section of its own after `.data`; placed
inside `.data`, ld's `--gc-sections` editing of it emits relocations at the
wrong offsets. A C program carries about 500 bytes of unwind tables from
libgcc. C++ costs far more: `cxx-test` has 765 KB of shared text and, per
process, 73 KB of unwind tables and 60 KB of other data, mostly vtables,
typeinfo and locale tables. Threads call pthreads directly: uClibc-ng claims
to be glibc 2.2, so gthreads would otherwise judge a program threaded by a
weak reference to `__pthread_key_create`, which LinuxThreads does not have
(`host-tools/toolchain-c33/gcc/patches/0003`). Atomics of up to 4 bytes are
inline and lock-free, masking interrupts as `testandset` does.

`libc` also builds `linux/artifacts/cxx-test`, which `app-test` runs from the
card after the thread test: exceptions through 40 frames with callee-saved
registers restored, through a 3 KB frame, rethrown, carried in an
`exception_ptr`, and thrown inside libstdc++; `bad_alloc`; RTTI and
cross-casts; static construction; iostreams, containers and `shared_ptr`;
and four threads throwing and catching concurrently under a mutex.

`rootfs` builds the root filesystem with Buildroot 2026.08 and leaves it as
`linux/artifacts/linux.img`, a 16 MB ext4 image with 4 KB blocks and no
metadata checksums. It goes on the card's FAT partition beside `linux.app`.

The kernel's own initramfs holds only `initramfs/rootstart`, a 2 KB
freestanding program, and the device nodes it needs before devtmpfs. It
waits for the card and mounts it with `usefree`, trusting the card's own
free-cluster count; otherwise attaching the loop device has FAT read and
scan its whole allocation table. There is no RTC with a backup cell, so it
sets the clock from the newest of `linux.img`, `linux.app` and `kernel.elf`
before anything is mounted read-write. `linux.img` changes whenever the
system writes, so the clock carries on from the last session. It then attaches `linux.img` to `loop0`
with direct I/O, so ext4's reads neither start the FAT file's readahead nor
get cached twice. It mounts the image `noatime`, credits the random seed BusyBox `seedrng` saved
last time, moves the card to `/mnt/sd`
inside it, mounts `/proc`, `/sys`, `/dev` and `/dev/pts` there (each a
BusyBox exec from `rcS`, about a tenth of a second), and becomes its
`/sbin/init`. If any step fails, it writes the
reason to the console and to `linuxboot.txt` on the card, then reboots to
the launcher. Crediting the seed makes the kernel's random pool ready before
the first program runs; otherwise the first read of `/dev/urandom` waits
while the kernel gathers jitter entropy, 3.4 s on this CPU. The seed is
renamed so that it is never credited twice. Twenty seconds after boot,
`/etc/init.d/late` runs `seedrng`, which mixes it in again uncredited and
saves a fresh one.
Shutdown does the same. A freshly built image carries a seed made at build
time. `inittab` unmounts or remounts everything read-only on
shutdown, and ext4's journal covers a power cut. Since the device is
switched off without unmounting, `/etc/init.d/late` freezes and thaws the
root after its seed save, which checkpoints the journal: a boot would
otherwise replay the last session's writes, 1.5 s of mounting on the
device, against 0.35 s now. The image has no orphan
file (`-O ^orphan_file`): mounting read its 32 blocks one at a time, each a
trip through ext4, the loop device, FAT and the SD host, 0.3 s of every
boot; orphans go in the superblock's list instead. ext4 is about 470 KB of
kernel code, which took the kernel's code past the 2 MB reach of a short
call, so the kernel is built with `-mlong-calls`.
`buildroot/patches/` adds the C33 as a Buildroot architecture, with an
external toolchain only. The `buildroot/external/` tree holds the defconfig,
the BusyBox configuration and two packages, `wr-console` and
`wikireader-system`. The latter installs `initramfs/`'s init configuration
and builds its freestanding diagnostics with the bare-metal compiler, along
with `/uclibc-smoke`, the ordinary uClibc program they run as their libc
check.
Buildroot builds with `c33-linux-uclibc-` from `toolchain`, and its FLAT
support links every program with `-Wl,-elf2flt`. The build starts from a
clean output directory each time, because Buildroot does not notice a rebuilt
C library. That takes about 16 minutes, most of it the host tools X needs
(Python for libxcb's protocol generator, CMake); a package's `-dirclean`
target and then `make -C` the output directory redoes one package in a
minute or two (`-rebuild` can leave a program unlinked against its rebuilt
libraries).

The image also carries `sl`, with ncurses and its terminfo. uClibc-ng
provides what Buildroot's own uClibc configuration offers packages, such as
the SUSv2 to SUSv4 legacy functions, `nftw`, GNU `glob`, `%m`, memory streams,
`wordexp` and `libutil`. It leaves out the shared-library loader, Sun RPC,
and `getcontext`, which has no C33 implementation.

BusyBox 1.38.0 is a static C33 bFLT with an interactive `hush`, core file and
text tools, checksums, archive/compression tools, filesystem inspection, and
recovery utilities, and `hush` carries `busybox/patches/`. It is installed as
`/init`, `/bin/busybox`, and a symlink for every applet. BusyBox init
starts the consoles only once `rcS` ends, so `rcS` does almost nothing: on
this MMU-less machine every exec, and every background job (the shell
re-executes itself for one), is about a tenth of a second, and `hush`
parses a whole `if` block or function before running it. It checks for
the PTYs. With `wr.selftest` on the command line it runs
`/etc/init.d/selftest`: the freestanding process, signal, and libc
diagnostics, the display and input checks, and a Hush and
file/text/archive tool suite, then `linux.ok` and, later, the
`linuxhw.txt` device report on the card. The tests pass `wr.selftest` on the
launcher's `init.ini` line, and the kernel appends it on a direct boot, the
bring-up and recovery path. `init` finally respawns an interactive `hush` on
`ttyC0`. `/diag-init`
remains available as the old freestanding rescue shell.

`wr-console` is the static bFLT framebuffer frontend. It uses only standard
fbdev, evdev, Unix98 PTY, devpts, process, and TTY interfaces; the application
contains no S1C33 register access or private kernel ABI. It is installed as
`/sbin/wr-console`, and BusyBox init supervises it beside the serial recovery
shell. Its shell's `PATH` ends in `/mnt/sd/bin`, so a program copied into the
card's `bin` folder runs by name.

Its terminal is 40 columns by 15 rows and implements the Linux console as
`TERM=linux` describes it to curses: cursor addressing and movement,
insertion and deletion of characters and lines, a scrolling region, reverse
video, saving, hiding and reporting the cursor, and xterm's deferred wrap in
the last column. Tab stops are every eight columns. Colours and other
renditions are ignored, since the panel has one bit a pixel. The console's
line-drawing characters are shown as `+`, `-` and `|`. The cursor is its cell
drawn inverted. Full-screen programs such as BusyBox `vi`, `less` and `top`,
and curses programs such as `sl`, therefore work. Escape is Ctrl+`[`.
`console/terminal-test.c` checks the escape handling on the build machine,
and the Buildroot package runs it before building the console.

The console holds VT 1 in process mode. When another program switches VTs,
the kernel asks the console first: it stops drawing (it keeps drawing into
its own copy of the screen), drops touches and keys, stops its blank and
suspend timers, and answers; when VT 1 comes back it repaints. Its VT's
keyboard mode is `K_OFF`, since the console reads the keyboards itself.

`startx [CLIENT] [-- SERVER-ARGS]` runs an X client on the panel: it starts
`Xfbdev` on the touchscreen and the buttons, with the classic grey weave
(`-retro`), without the smart scheduler's 20 ms `SIGALRM` (`-dumbSched`),
and with 100 ms scheduling slices instead of 5 to 15: a client's requests
are served until they run out or the slice does, and on this CPU one arc
fill takes milliseconds, so the short slice ended partway through xeyes'
redraw and the idle handler put the half-drawn eyes (the old pupil cleared,
the new one not yet drawn) on the panel, waits
for its socket, runs the client, and stops the server when the client exits,
which gives VT 1 back to the console. With no client it runs twm and xeyes.
Dragging on the background moves the pointer; holding still there for 0.6 s
opens twm's menu (`buildroot/external/patches/xapp_twm`: twm opens menus
only from buttons, and a touch is its only button), whose Exit ends the
session. twm's configuration (`wikireader.twmrc`)
uses the server's built-in `fixed` font and places windows itself, since
placing one by hand is awkward by touch. Menu commands start through
`system()`, which uClibc does with `vfork`. xeyes asks for XInput 2.2 rather than 2.0
(`buildroot/external/patches/xapp_xeyes`): a 2.0 client gets no raw motion
while another client has the pointer grabbed, and twm has it grabbed for any
drag that starts on the background or a frame. It also drops raw motion
queued behind the event it is drawing, since drawing asks where the pointer
is now. It moves the pupils along (dx, dy) over its length instead of
through `atan2`, `cos` and `sin`, which were most of its arithmetic.

A drag under twm costs about 46 ms of CPU a frame in wremu, so the eyes
follow at about 20 frames a second and a 33 Hz touch stream runs ahead of
them. The software cursor is about a fifth of that (`startx -- -nocursor`
leaves it out), xeyes about a seventh, and the kernel over a third: each
frame is about 57 system calls (xeyes' Xt and xcb make 37, most of them
reads that find nothing), 14 context switches and 10 timer ticks, and the
scheduler's load tracking, 64-bit arithmetic run 39 times a frame, is a
third of the kernel's share. `Xfbdev` is the kdrive framebuffer server of xorg-server 1.19,
the last release that has it (`buildroot/external/package/xserver-kdrive`).
Its patches let it `vfork` where it would `fork`, load a keymap `xkbcomp`
compiled at build time instead of running `xkbcomp` (the image carries no
XKB rules or sources), draw through a shadow copied to the panel bit-reversed
(X keeps the leftmost pixel in a byte's low bit, Linux framebuffers in the
high one), and take a touchscreen as an absolute pointer, the touch as its
first button. The client libraries are static (`buildroot/patches/0002`),
with libX11's loadable modules turned off. `fbFillSpans` finds each
span's clip band by bisection (patch 0005) instead of walking every clip
box: a shaped window such as xeyes' has a box or two a row, and the walk
was most of the cost of filling its pupils. libX11 is built without its East Asian
multi-byte charsets (`buildroot/external/patches/xlib_libX11`), whose tables
were 410 KB of every X program, and libXfont2 with only its built-in fonts
(`buildroot/patches/0003`: no FreeType or font-file readers, 575 KB of the
server). `Xfbdev` is 1.6 MB, twm 1.1 MB and xeyes 1.0 MB; startup is mostly
reading them from the card.

`drivers/mmc/host/s1c33-sd.c` powers and pin-muxes the WikiReader card slot,
identifies SDSC and SDHC cards, and exposes standard devices such as
`/dev/mmcblk0p1`. The card is the only device on the S1C33's synchronous
serial interface, so the driver is an MMC host in SPI mode that owns the
interface itself, instead of a generic SPI controller under `mmc_spi`. Its
protocol handling (commands, responses, data tokens, write handshakes, CRC
retries) follows `mmc_spi`; its transport is the controller's registers.
Under `mmc_spi` each 512-byte block cost about four SPI messages, each
validated, accounted, chip-selected and scheduled, plus a DMA interrupt that
put the reader to sleep: about 1.9 ms a block on this CPU against 0.3 ms on
the wire. Here every character is 32 bits, commands and tokens included:
outgoing bytes are packed into words behind all-ones padding, which the card
ignores, and the bytes of each word received queue up for whoever reads
next. Each data block is one HSDMA2/HSDMA3 transfer through DMAengine: the
request's scatterlist is mapped once with the DMA API, HSDMA3 fills each
block from the receiver while HSDMA2 feeds all-ones to the transmitter from
a descriptor the host prepares once and resubmits, and the CPU polls for
the end. A token lands anywhere in a word, so the words HSDMA writes are
byte-swapped and moved up to the token's alignment in one pass over the
block. Unaligned buffers are read through the byte queue.

The controller is `drivers/dma/s1c33-hsdma.c`, a DMAengine provider for
its four channels. Each channel has trigger sources of its own (only
channel 3 answers the SPI receiver, only channel 2 its transmitter), so a
request line is a channel and a trigger together, named in the board's
`dma_slave_map`. Completion is polled through the channel status, which
retires the descriptor: a CPU sleeping for the interrupt would execute
`HALT`, which on physical E07 parts stops SPI-triggered HSDMA. The status
also gives the residue of a transfer under way, from the channel's count,
and a descriptor built with `dmaengine_prep_interleaved_dma()` can hold its
memory address fixed. On this core, which fetches every instruction outside
a short loop from SDRAM, generic per-transfer code is dear, so the provider
keeps its own descriptor lists, recycles descriptors and skips register
writes that would not change anything. The card runs at MCLK/4, 15 MHz
under Grifo, as Grifo and the original firmware run it, and block CRCs are
checked unless `mmc_core.use_spi_crc=0`.

Multiple-block reads are streamed. The card sends each block's gap, token,
data and CRC one after another and simply waits whenever the host stops
clocking, so after the CPU has found the first token, the read comes in by
DMA into a 64 KB ring: the receive channel writes the wire's bytes there as
words, and the transmit channel sends all-ones from a single word in IVRAM.
Each transfer runs to the ring's end or to the first byte not yet used, and
the next starts as soon as it ends and there is room: the CPU looks when it
should have ended, after each block, and between requests a high-resolution
timer does. The CPU follows behind, finding each token and
putting each block in order and checking it on the way to its place in the
request, as far as the residue says has come in (a block that wraps is put
together first). Nor does the stream end with the request: the card is left
reading on and the stop command is answered without being sent. So while
the kernel copies one request's
blocks out, the next request's cross the wire, and a request that reads on
from there, as sequential reads do, finds them waiting (41 KB of them on
average in a raw read on the device). Any other request, a new clock or
power state, or an error stops the stream and sends the stop command. While
a transfer runs the idle loop polls instead of halting
(`cpu_idle_poll_ctrl()`), since HALT would drop the port's DMA requests for
good. Requests are up to 128 KB. A raw 4 MB read through Grifo takes 2.87
to 2.89 s on the device (1.46 MB/s, 78% of the wire), against 3.14 to 3.20
s with a buffer that stopped at its end, 3.54 s with the stream ending at
each request and 5.1 s a block at a time; the driver's share of a block is
13,000 cycles against 16,500 on the wire. The check is
`sd_unpack_crc()`, which takes each word in wire order into the CRC four
bytes at a time and swaps and shifts it into place: about 37 instructions a
word, too long for the fetch queue, so it runs from A0 RAM (the zero-wait
on-chip RAM Grifo leaves applications from 0x0c00; the kernel copies
`__iramfunc` code there at boot, see `asm/iram.h`), with its 2 KB of tables
there too, in about 9,700 cycles a block on the device.

Single blocks, and cards that shift a token by a few bits, are read a block
at a time, pipelined: once block N+1's token is in and its transfer
started, block N is put in order and checked while N+1 crosses the wire.
There a block's transfer takes about 19,600 cycles on the device against
16,400 on the wire, and that is the DMA's own time, not the CPU's use of
the bus: writing to IVRAM instead of SDRAM, grouping the check's SDRAM
accesses, or checking only after the transfer did not shorten it. A
stream's words cost about 13 cycles each over the wire, so most of the
rest is a transfer getting going.
Writes and shifted blocks use the byte-at-a-time CRC, a 26-byte loop
compiled with `-falign-loops=16` so that it runs from the fetch queue, on the
same tables. The all-ones the transmit channel sends are in IVRAM, above the
framebuffer, where its reads close no SDRAM rows. Disabling the serial block
while it drives SCLK creates a real stray edge, and the block has to be
disabled to change its clock or character size. With one character size,
only a new clock rate does that, a few times a boot, and SCLK sits in the
`hold` pin state, a port output at its idle level, across it. The switch
through the pin-control core costs about 0.4 ms on this CPU, which is why it
could not stay per block: two character-size changes a block made card
reads two and a half times slower. The driver takes the slot's active-low
chip select from the board's software node. The
card's 3.3 V rail and the level buffer between it and the S1C33 are two
GPIO-switched fixed regulators that the driver consumes as `vmmc` and
`vqmmc`; the settling time before the buffer may drive and the off time
before the rail may return are regulator constraints rather than sleeps in a
board callback. After power-up the host resets the card until it answers
that it is idle, then hands it to the MMC core: `mmc_spi` instead waits
for the card to stop signalling busy, but a card not yet in SPI mode
drives nothing and its data line reads zeros on this board, so that wait
ran out its timeout, and the core's single reset does not always take on
the device's card. The card is non-removable, since the system runs from it,
so the MMC core does not poll it.
The DMA provider's probe establishes a documented reset-equivalent HSDMA
state: it disconnects every trigger, stops all four channels, clears their
trigger and terminal-count latches, turns IDMA off, and gives the DMA
channels no interrupt priority. The kernel includes FAT/VFAT and mounts the first
partition at `/mnt/sd` with synchronous writes. Under `wr.selftest` early
userspace leaves `linux.ok` there as a persistent, serial-port-free boot
report.

The kernel's `memset`, `memcpy` and `memmove` (`arch/c33/lib/string.S`) run
from A0 RAM too. Loads and stores share the SDRAM controller's one data row,
so a copy that alternates them opens a row for every access, which the
device measured at 6.6 cycles a byte. The aligned copy moves eight words at
a time instead, eight loads then eight stores, which the device measured at
4.0; the loop is longer than the fetch queue, hence A0 RAM, and every short
call gains from not fetching its code from SDRAM. The exported names are
stubs that jump through a pointer, to the SDRAM copies until `setup_arch()`
has moved them. A 512-byte copy to user space takes about 2,500 cycles in
wremu against 3,700; a raw 4 MB card read 4.98 s against 5.17 in wremu, and
5.29 s against 5.36 on the device. Division
(`arch/c33/lib/div.S`) is there as well: the PE core has no divide
instructions, and libgcc's `__udivsi3` and the generic `__div64_32` behind
`do_div()`, which the scheduler's load tracking calls on every enqueue and
tick, were 4.4% of the boot from SDRAM. The replacements shift and subtract
once per quotient bit in registers, and take 2.3%. `echo 1 >
/sys/module/iram_bench/parameters/run` times the same loops from SDRAM and
from A0 RAM and logs cycles per unit, to check wremu's model of both against
the device; `check` runs it. On the device, arithmetic takes 3.59 cycles an
instruction from SDRAM and 1.31 from A0 RAM, and wremu is within 10% on most
cases, but it makes back-to-back stores from A0 RAM 16% too cheap, streamed
loads from SDRAM 13% too cheap, and short branchy loops such as division up
to 37% too dear. Likewise `echo 1 > /sys/devices/platform/s1c33-sd/read_timing`
starts per-phase counters for card reads, and reading it gives cycles a
block for the tokens, the transfers' setup, the preparation of the next
block's, the check, the wait, the status call, the CRC bytes and the rest
of each request, and for streamed reads the transfers, gap bytes, errors,
the requests that carried on and what they found in hand, with commands
and writes; `s1c33_sd.timing=1` on the command line starts them at boot,
and `check` prints them, then runs a timed read. With `printk.time=1` on
the `init.ini` line `check`'s kernel log is a boot timeline, which
`wr-console` ends with the uptime at which its prompt came up; with
`bootlog.on` on the card Grifo appends each application's load to
`bootlog.txt`, with its reads and how long opening the file, building the
cluster map, reading the sections and zeroing took. wremu's DMA model was changed after them (`dma_async`), and its
driver total per block for a stream was within 1% of the device's. It is
too harsh on a stream left running while the kernel copies: its default
`dma_cpu_penalty=15` holds the DMA back behind the copy's SDRAM accesses
until little comes in (3.73 s), where the device keeps it going; with
`WREMU_MODEL=dma_cpu_penalty=0` it gives 2.74 s against the device's 2.87
to 2.89 s.

`boot-test` runs on macOS. It builds a temporary FLASH image and a FAT32 card
holding Grifo, `init.app`, `linux.app`, `linux.img` and a single-entry
`init.ini` that passes `loglevel=7 wr.selftest`, boots the device's own chain
at Grifo's 60 MHz, and requires the
BusyBox PID 1 startup and one-shot diagnostic suite to complete without a
kernel panic. It then injects an `echo` command into the real `hush` over UART0
and verifies its output. It separately generates panel taps for a command and
Enter key, requires UART1 serial-core and serdev to deliver evdev records, and
requires the userspace frontend to execute that command through its PTY-backed
Hush. The test also checks the final display image for console text and the
three keyboard rows. The card and emulator
display output are kept outside the checkout and removed afterward. The card
is writable only for this isolated run; after the guest exits, the
host parses its raw FAT image and requires `linux.ok` to contain the expected
status. A console claim without persisted card bytes therefore fails the test.
The same regression requires the SD host to announce streamed HSDMA block reads, the
DMA provider its polled channels, and the emulator to report nonzero HSDMA2 transmit and HSDMA3 receive
activity. It also
checks fbdev geometry, reads and rewrites the complete `/dev/fb0` image, and
requires the frontend to receive the scripted panel events from
`/dev/input/event0`.

## What comes next

Richer keyboard modes, console session management, and power management can
grow around the proven LCD, touch, PTY, storage, and recovery userspace
paths, and an fbcon on the framebuffer would inherit the keyboard devices the
frontend now publishes.
