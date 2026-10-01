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
16 MiB production boards and the 32 MiB early ones.

The board is a device tree, `arch/c33/boot/dts/wikireader.dts` over the
chip's `s1c33e07.dtsi`, built into the kernel (`GENERIC_BUILTIN_DTB`), since
Grifo loads the kernel alone; it has no `/memory` node and no bootargs,
which the SDRAM controller and the launcher supply. Every driver matches by
`compatible`, and each device finds its clocks, pins, GPIOs, supplies, DMA
channels and PWM through the tree's references, which also order the
probes. The device-tree core costs the kernel about 100 KB. The S1C33
interrupt controller is `drivers/irqchip/irq-s1c33.c`, an irqchip behind a
linear irqdomain whose hardware interrupt numbers are the trap vectors. The
tree names an interrupt `<vector priority>`, and the controller sets the
priority nibble of the vector's group, so no driver writes the controller's
registers; a source starts with its pending flag cleared. The trap entry
routes each vector to the domain rather than treating it as a Linux IRQ
number. The domain's
allocator prefers the hardware number when it is free, so `/proc/interrupts`
still reads in vectors. The timer and both UARTs use normal `request_irq()`
registrations visible there. Linux runs the scheduler, registers the
interrupt-driven `ttyC0` UART console, and runs BusyBox 1.38 as PID 1.
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

Under `wr.selftest`, `/usr/libexec/wr-selftest` checks the kernel's process
paths from userspace. It `vfork()`s (the asm-generic
`clone(CLONE_VM | CLONE_VFORK)` ABI), executes itself as a child, and reaps
its exit status with `wait4`: the live syscall register frame, task
creation, scheduling, exec, exit, and parent wakeup. It installs a `SIGUSR1`
handler, signals itself, and returns through the C33 `rt_sigreturn`
trampoline; the kernel saves and restores the complete integer context,
signal mask, and alternate-stack state in an aligned `ucontext` frame on the
userspace stack. Handler entry emulates the four-byte return-address push
of a normal call: `SP % 16 == 12`, with the argument base at `SP + 4`
aligned to sixteen bytes. `signal-entry.S` checks this before the C prologue,
including nested delivery on an alternate stack; the sigreturn trampoline
then runs with the return-address word already popped.
Exception entry checks an active alternate signal stack against its own
lower bound, so placing it below the ordinary stack does not trigger a
false overflow diagnosis.
Traps reach the kernel through `CONFIG_GENERIC_ENTRY`, so
tracing, seccomp, and audit see every system call and the exit path is the
generic one: a child that calls `PTRACE_TRACEME` before `execve()` must be
stepped through several `PTRACE_SYSCALL` stops before it reaches its exit
status, instead of running straight there. Last, it checks the C library's
start, `printf()`, `getpid()` and `setjmp()`/`longjmp()`.

The separate signal regression checks interrupted reads with and without
`SA_RESTART`, asynchronous return to userspace instructions with live
registers and TLS, interrupted relative and absolute sleeps, and `ppoll`.
Nested `SA_ONSTACK` handlers verify the alternate stack, `SA_SIGINFO`
payloads, `ucontext` signal masks, and mask restoration on `rt_sigreturn`.
Independently executed children are stopped and continued during timed
waits. Restart-block waits enter `restart_syscall`, preserving the original
deadline instead of starting a new relative timeout. A 600 ms sleep that
took about 958 ms before this fix now takes about 603 ms in wremu.
`ppoll` retains Linux's different behavior, excluding time spent stopped.
The same regression also runs on native Linux to check these expectations.

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
driver. The board device tree describes the touchscreen as a child of UART1.
Upstream serdev enumerates that child, matches its `compatible` property
against the driver's normal OF table, and binds it through the driver core.
The node also carries the standard `current-speed` and touchscreen dimension
properties consumed by the driver. Keyboard geometry, labels, press state, and
character translation are entirely userspace policy: the touchscreen driver
does not know about keys or TTYs.

The three front buttons on P60..P62 and the power switch on P03, all of
them pressed high, are a `gpio-keys` device in the device tree,
reporting `KEY_F1` (random), `KEY_SEARCH`, `KEY_BACK` (history) and
`KEY_POWER`. They interrupt through the port block: key input 0 compares
P60..P62 with a stored pattern, and port input 3 watches P03 for the edge
opposite its level. The switch's polarity came from the board: described as
active-low, every resume reported a fresh press, because a suspend releases
all keys and the next report re-reads a pin that was never released, and
the machine slept again four seconds after each wake.

Pins belong to `drivers/pinctrl/pinctrl-s1c33.c`, a pin controller for ports
0 to 9 that is also the GPIO chip for ports 0 to 6. Every pin is a group of
its own with the manual's functions for it (`pins = "P11"; function =
"tm1";`), and a peripheral's pins are also a named group whose function
gives each pin its signal (`groups = "spi"; function = "spi";` for P65 to
P67), one node for the state instead of one a pin. The driver core
applies each device's default state before its probe, and requesting a
line as a GPIO selects its port function, so nothing else writes port
registers. The SPI flash's chip select, which shares the card's bus, is
held high by a GPIO hog. The generic `output-low` configuration parks a pin as a port output,
which is how the SD host holds its clock still.

The panel's contrast is a PWM: the firmware runs timer 1 at MCLK/4096 with
comparison A as a 12-bit contrast number, 0 lightest and 4095 darkest, and
leaves it running. `drivers/pwm/pwm-s1c33.c` drives one timer channel as a
PWM chip, adopting the running configuration rather than restarting it and
moving only comparison A when the period is unchanged so the output keeps its
phase; it never touches the ADVMODE and PAUSE registers it shares with the
clocksource. `drivers/video/backlight/wikireader_lcd.c` consumes it through
the tree's `pwms` and exposes the firmware's number as
`/sys/class/lcd/wikireader/contrast`, reading the value back from the PWM at
probe so a boot changes nothing on the panel. Timer 1's CMU gate is a clock
the PWM driver holds, which is what keeps the panel lit once the clock core
turns off every gate nobody claimed.

The battery and the board temperature come from the chip's 10-bit A/D
converter, `drivers/iio/adc/s1c33-adc.c`, an IIO device that converts one
channel per read against AVDD, a fixed 3.3 V regulator it takes as `vref`.
Everything above it is a stock driver wired by the tree's `io-channels`
references. AIN0 sees the two AAA cells through a 150k/1M divider, which
`iio-rescale` undoes as a `voltage-divider`, and `generic-adc-battery`
publishes the result as `/sys/class/power_supply/battery`, with
`voltage_now` in microvolts and `status` Discharging. AIN1 is a 100k NTC
thermistor under a 120k pull-up to the same rail; `ntc_thermistor` reads it
as `/sys/class/hwmon/hwmon*/temp1_input` in millidegrees. The part, a
TCT6GJ104H410, is not in that driver's tables, so the board names the Murata
NCP03WF104, another 100k thermistor with B = 4250 K. AIN2, the panel's V4
bias, is a plain IIO channel. `patches/0014` makes `generic-adc-battery`
treat a battery nothing supplies as discharging rather than charging.

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
GPIO descriptor taken from the same device tree as the SD slot's chip
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

Everything on the root filesystem is FDPIC ELF with shared libraries (see
`host-tools/toolchain-c33/gcc/ABI.md`), loaded by `binfmt_elf_fdpic` and
uClibc-ng's `ld.so`. Each module, the program and every library, has a text
segment with no absolute address in it and a data segment of its own, which
its code reaches through `%r15`. A call into another module goes through a
function descriptor, its entry point and its `%r15`. The kernel maps each
module's text read-only, and no-MMU Linux lets read-only private mappings of
one file overlay each other, so every process running BusyBox, or linking
libc, shares one copy. The system is on ext4, which cannot map files
directly, so the first use of a module reads its whole text into RAM: libc's
480 KB is read at the first exec of the boot, where a static program read
only the parts it used. The BusyBox suite checks that PID 1 and a child map
the same `/bin/busybox` and `libc` text.

A library's own addresses are most of its relocations -- libc has 1,212 of
them, redone in every process -- so they are `R_C33_RELATIVE`, which the
linker puts first in `.rela.dyn` and counts in `DT_RELACOUNT`, and `ld.so`
runs them through a loop of their own that relocates an address with one
comparison instead of a search of the load map. The others go through a
loop that makes no calls, so its state stays in registers, as long as their
symbol has been looked up; a symbol named by many relocations, such as
`__stack_chk_guard` in 218 of BusyBox's, is looked up once a module, into a
256-entry table on the stack (libc's relocations name 59 of its 1,687
symbols). TLS relocations reuse the same cache, including symbols at offset
zero: libc's 401 references to `errno` need just one lookup. The PE core has
no divide, so a lookup's remainder by the bucket count is a multiply by a
reciprocal, and `ld.so` is built at `-O2`, whose loops branch once an
iteration. Single-threaded `/bin/true` spawn/exec/wait takes about 64 ms in
wremu, half of it in the kernel's process creation and teardown.
Calls through the `.plt` are bound at their first call.
Libraries bind as ELF has them, without `-Bsymbolic`: a program may define a
function a library also defines, and the library then calls the program's. Without an MMU there is
nothing for RELRO to protect, so the image is built without it, which would
otherwise bind every call at exec.

A program's stack cannot grow without an MMU: the kernel allocates the size
the link asks for, in `PT_GNU_STACK`, whole at exec. The compiler asks for
32 KB. 16 KB was too little for the X clients,
since a lazily bound first call adds `ld.so`'s resolver at whatever depth it
happens. An overflow writes silently into whatever lies below the stack --
twm's overwrote the X server's function descriptors -- so the kernel checks
the stack pointer at each exception from an FDPIC program, names the program
and kills it if the exception frame would land below its stack.

The kernel's own `/init`, `rootstart`, has no C library but is a static
FDPIC program all the same (`initramfs/build-freestanding.sh`): its entry
point relocates it with uClibc-ng's `crtreloc.o`, as the C library's does,
and its system calls are veneers of its own. The kernel has no other binary
format.

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
  `-mc33pe -mfdpic` with short calls, which reach any module under 2 MB (the
  largest, Xfbdev, is 1.1 MB), defines `__uClinux__`, and links FDPIC
  executables against uClibc-ng's shared libraries in its sysroot, with
  `/lib/ld-uClibc.so.0` as the interpreter; `-static` links a static FDPIC
  program and `-shared` a library. `-Wl,-z,stack-size=<bytes>` sets a
  program's stack.

The userspace compiler is built against the kernel's UAPI headers and
uClibc-ng's headers, so `fetch` comes first.

The tests boot Grifo, `init.app` and the MBR flash from `samo-lib`. Build them
with the repository's normal firmware targets if they are not present.

## Core dumps and host symbolization

Fatal signals now produce standard ELF32 C33 FDPIC cores. PID 1 starts with
a 4 MiB soft `RLIMIT_CORE`, inherited by services and shells. `rcS` sets
`core_pattern` to the small `/usr/libexec/wr-core` pipe helper and
`core_pipe_limit=1`, so the kernel preserves `/proc/PID` until it finishes.
The helper streams through a 16 KiB buffer directly to the SD card and saves
only the latest crash:

* `/mnt/sd/crash.elf`: registers and process memory in the kernel's ELF core format.
* `/mnt/sd/crash.map`: the crashing process's `/proc/PID/maps`.
* `/mnt/sd/crash.txt`: executable path, PID, signal, time, rootfs build ID,
  requested limit, saved bytes, and `complete`, `truncated` or `error` status.

The helper caps each capture at 4 MiB, honors smaller limits, and leaves the
previous capture alone when the soft limit is zero. Piped dumps bypass the
kernel's own size enforcement, so this cap is enforced while copying. It
drains the rest of an oversized stream without storing it. A truncated core
can still supply registers and resolve its PC, though memory near the end
of the dump may be missing. A second concurrent crash is skipped while the
collector is active. These files contain process memory; copy all three off
the card before another crash overwrites them. Disable captures in a shell
with `ulimit -S -c 0`, and restore the default with `ulimit -S -c 8192`
(BusyBox expresses core limits in 512-byte blocks).

Rootfs builds enable optimized DWARF builds, save the matching unstripped
ELFs under `linux/artifacts/symbols/` with the target's directory layout, and
remove debug sections from on-card files. Libc uses `-g` without uClibc's
`DODEBUG`, which would also disable optimization and change runtime behavior.
Keep the symbol directory with its corresponding `linux.img`: the host tool
rejects a rootfs build-ID mismatch. For your own SD-card programs, retain
their unstripped ELFs at the corresponding path under the symbol directory,
for example `symbols/mnt/sd/myprog`. Custom programs must match their cores
too; the rootfs ID does not identify separately copied executables.

After copying the capture off the card, run this on the Mac:

```sh
./linux/debug/core.py /path/to/crash.elf
./linux/debug/core.py /path/to/crash.elf --stack 64 --address 0x08123456
```

The tool reads the C33 register notes and executable/interpreter FDPIC load
maps, and translates shared-library addresses through the saved memory map
and each ELF's `PT_LOAD` file offsets. Segments can relocate independently;
subtracting one base address from every PC would be wrong. The host's
`c33-epson-elf-addr2line` prints function names, demangles C++, and resolves
source lines and inline calls. Use `--symbols DIR` for an archived bundle
and `--addr2line PATH` for a different host binutils installation.
`--stack` reports stack words pointing into executable mappings as possible
call sites; it is not a stack unwinder or a replacement for GDB.

`make -C linux core-test` builds crash fixtures and boots through the real
Grifo menu. It checks actual misaligned-load SIGBUS faults in an executable
and a separate shared library, resolves both fault PCs to source lines on
the host, tests zero and small limits, and requires the shell to survive
and reboot back through Grifo. Captures and reports remain in
`linux/artifacts/crashes/` for inspection. A NULL load is not a reliable
crash fixture on a machine without memory protection.

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
make -C linux signal-test
```

`fetch` reconstructs the pinned upstream kernel, uClibc-ng and Buildroot
revisions from `revisions` on the VM disk, applies their patches, then
installs their overlays. Each kernel build also refreshes `overlay/` in the VM source tree so
iterative port changes cannot be silently missed. It leaves the final kernel
in `linux/artifacts/`, along with the stripped `linux.app` and its Tux launcher
icon. `app-test` boots the real Grifo menu in the emulator, taps that icon,
requires Linux and BusyBox to start, then uses the standard reboot syscall and
requires Grifo's watchdog reset to return to the menu.

`app-test` includes the signal regression. To run it alone against already
built artifacts, use `python3 linux/signal-test.py`; its boot log is saved
in `linux/artifacts/signal-test-run/`. `--kernel` and `--binary` select saved
builds for comparisons, and `--output` selects a separate log directory.

`libc` builds a no-MMU C33 uClibc-ng, shared and static, with `ld.so`, the
native asm-generic syscall ABI and time64 interfaces, and installs it with
the kernel headers into the `c33-linux-uclibc-` sysroot. Its link regression
compiles a real `stdio.h` program both dynamically and statically.
Every program start sets a stack-protector guard from the clock
(`SSP_QUICK_CANARY`). Without an MMU the guard only catches bugs, and the
default of reading `/dev/urandom` would make the first program at boot wait
3.4 s for the kernel's random pool.

The library includes NPTL and native ELF TLS on the NOMMU C33. Threads share
one process ID and have separate kernel thread IDs, with `CLONE_SETTLS`,
futex joins, per-thread `errno` and compiler `__thread` variables. All four
ELF TLS models work in executables and shared libraries, including modules
loaded with `dlopen` after threads have started. The general-register ABI
is unchanged: no register is reserved for TLS. GCC and libc read the
current TP inline from a reserved word at `0x00001fbc` in on-chip A0 RAM.
The compiler build copies its definition from the kernel's `asm/tls.h`,
so code generation and the kernel use the same ABI constant.
Reading the thread pointer needs neither a helper call nor a syscall;
dynamic TLS still uses `__tls_get_addr` to find its module's allocation.
The ABI and relocations are in `host-tools/toolchain-c33/gcc/ABI.md`.

Default worker stacks are 64 KB; the stack cache retains at most 256 KB,
instead of the upstream 16 MB default. Stack guards cannot protect memory
on this machine. NPTL uses the backend's interrupt-masking inline atomics
and kernel futexes, and remains part of libc. Cancellation uses shared
`libgcc_s.so.1`, loaded on the first cancellation in ordinary C programs;
C++ programs that need exception handling load it at startup.
The unwinder discovers module frames through
`dl_iterate_phdr` and `PT_GNU_EH_FRAME` headers.
The linker emits sorted FDPIC search tables in the shared read-only segment.
The unwinder searches with the PC's link-time address, then maps only the
selected frame into the separate data segment. Lookup finishes inside the
serialized program-header callback; it allocates no FDE index and needs no
runtime sorting. Tables cost 12 bytes plus 8 bytes per FDE, shared between
processes. Older modules without search tables fall back to linear scanning.
The large C++ test's first throw fell from about 108 ms with runtime indexing
to about 6 ms, while repeated libstdc++ throws remain about 3.8 ms in wremu.
`libc` builds the optional runtime after
installing libc, then installs it in the sysroot.
`rootfs` includes the runtime and matching host symbols automatically.
The post-runtime build checks that the arithmetic archive contains no
unwind objects, that the dynamic C++ test uses the shared unwinder, and that
its search tables are sorted, read-only and free of runtime relocations.
Contended spinlocks yield on this single core instead of spinning until
the scheduler preempts them; ordinary mutexes remain the usual choice.

`python3 linux/runtime-bench.py` measures mutex lock/unlock and actual
`/bin/true` spawn/exec/wait through Grifo. Its `--image`, `--kernel` and
`--binary` options allow comparison against saved builds; emulator timings
are diagnostics rather than hardware performance guarantees.
Use `--argument=--single` to leave the benchmark process single-threaded.
With `--profile-workload exec --profile linux/artifacts/exec-profile`,
capture the 32 spawn/exec/wait iterations instead of the mutex loop;
pass `--iterations 32` to `profile-report.py` for that capture.
Use `--profile linux/artifacts/mutex-profile` to capture just the marked
mutex loop, then `python3 linux/profile-report.py linux/artifacts/mutex-profile`
to attribute its instructions, MCLK cycles and fetch waits using guest
load addresses and matching ELF symbols. For exceptions, use
`--binary linux/artifacts/cxx-test --argument=--bench`; its profile window
covers repeated throws through four extra frames. Pass the same `--binary`
and `--iterations 32` to `profile-report.py` for that capture. Keep the
benchmark ELF and `linux/artifacts/symbols` from the profiled build.

`libc` builds `linux/artifacts/pthread-test`, `pthread-test-static` and
`nptl-test`, with two small TLS libraries. `app-test` runs them from the
SD card: contended mutexes,
condition variables, semaphores, once initialization, thread-specific data
and destructors, TLS initialization/alignment/isolation, thread identity,
signals, robust mutex owner-death recovery, cancellation cleanup,
contended spinlocks and exec clearing TP before libc initializes it.
The suspend regression starts two threads before s2idle, blocks on the
touchscreen without a timer, then checks their original thread pointers,
TLS values and `errno` after touch wakes the machine. The C++ regression
also throws through a shared library across repeated `dlopen`/`dlclose`.

uClibc-ng has wide characters, which libstdc++ and many packages need; they
are about 8 KB of libc's text. `libc` then builds libstdc++
against the installed C library, with `toolchain.sh libstdc++`.

That step also builds GCC's upstream `libatomic` against the installed
libc; `toolchain.sh libatomic` rebuilds just that runtime and its C test.
The static archive lives beside `libstdc++.a` in the cross toolchain and
is imported into Buildroot's staging sysroot. Link programs that need it
with `-pthread -latomic` after their objects. There is no extra shared
library to install on the card: only used code is linked into a program.
Buildroot recognizes the C33 FDPIC toolchain as providing libatomic, so
packages requiring atomic intrinsics can be selected.

C++ exceptions unwind with DWARF tables. The tables hold absolute
addresses, so they live in the data segment, where the loader relocates
them. Dynamic modules use program-header discovery; static programs
register their frames with `crtbeginT.o`. A personality routine is
reached through a word holding its function descriptor. `.eh_frame` is an output section of its own after `.data`; placed
inside `.data`, ld's `--gc-sections` editing of it emits relocations at the
wrong offsets. A C program carries about 500 bytes of unwind tables from
libgcc. C++ also carries unwind tables, vtables, typeinfo and locale tables.
Both libc and C++ use the shared unwinder so cancellation runs C++
destructors. Ordinary arithmetic helpers remain in the static libgcc
archive. Threads call pthreads directly: uClibc-ng claims
to be glibc 2.2, so gthreads would otherwise judge a program threaded by a
weak reference to `__pthread_key_create`, which the earlier LinuxThreads did not have
(`host-tools/toolchain-c33/gcc/patches/0003`). Atomics of up to 4 bytes are
inline and lock-free, masking interrupts around their read/modify/write.
The runtime supplies 64-bit operations and generic aggregate load, store,
exchange and compare-exchange. These use GCC's POSIX pthread-mutex fallback
and report that they are not lock-free; do not use them in signal handlers
or assume the locks synchronize separate processes. This unblocks C11
`_Atomic uint64_t`, C++ `std::atomic<uint64_t>` and wider atomic records,
without requiring an MMU.

`libc` also builds `linux/artifacts/atomic-test`, run from the card by
`app-test`: 64-bit arithmetic and bitwise operations, compare-exchange
success and failure, carry across the 32-bit boundary, lock-free queries,
and four threads contending on counters and a 24-byte aggregate spanning
the runtime's lock-table boundary. The C++ test includes contended 64-bit
`std::atomic` operations, initialized `thread_local` values and destructors,
and cancellation through a blocked semaphore wait with RAII cleanup.

Local IPC is available through POSIX shared memory and message queues and
System V shared memory, semaphores and messages. `rootstart` mounts
`/dev/shm` and `/dev/mqueue` with sticky, world-writable directories and
`nosuid,nodev`; `fstab` records them too. The NOMMU kernel uses its small
ramfs-backed implementation for `tmpfs`, including shared mappings. It has
no swap or tmpfs size quotas: allocate bounded buffers, truncate a new
shared-memory object to its final size before mapping it, and unlink it
when finished. A shared mapping needs contiguous physical memory, so it
can fail under fragmentation even when total free memory looks sufficient.

C33 implements `mlock`, `munlock`, `mlockall`, `munlockall` and `mlock2`
in the no-MMU kernel. RAM is already resident; these calls validate ranges,
enforce `RLIMIT_MEMLOCK`, track locks per process, and prevent `MS_INVALIDATE`
on locked pages. `MCL_FUTURE` also charges new mappings. Overlapping locks
are idempotent, and unmap/exec clear state. `/proc/PID/status` reports `VmLck`.
The lazy bitmap costs 1 KiB per locking process on the 32 MiB machine.
Applications compiled with the old inline libc no-ops must be rebuilt, and
the new libc wrappers require the matching kernel. The LTP coverage and
remaining limitations are documented in [ltp/README.md](ltp/README.md).

Repeated no-MMU mappings of the same physical range retain separate ownership
records. Each unmap removes one record; the last clears the mapping and locks,
and process exit releases every remaining record and callback. The kernel
enforces `/proc/sys/vm/max_map_count`, including these exact aliases.
Nonidentical overlapping mappings return `ENOMEM` while preserving the live
mapping; `/proc/PID/maps` shows each physical address range once.

Link programs using `shm_open` or `mq_*` with `-lrt`. System V IPC is in
libc; BusyBox provides `ipcs` and `ipcrm` to inspect and remove its objects.
The C33 libc patch makes the split-time kernel IPC structures match its
64-bit `time_t` ABI, and converts timestamps only for successful stat
commands, avoiding writes beyond the shorter information structures.
NPTL supplies process-shared POSIX mutexes, condition variables and
semaphores through kernel futexes; place the objects in genuinely shared
memory. The process-private libatomic fallback remains unsuitable for
cross-process synchronization.
The kernel patch forwards the backing file's NOMMU mapping capabilities
through the System V shared-memory wrapper; without it `shmat` rejects
the ramfs-backed segment with `ENODEV`.

`libc` builds `linux/artifacts/ipc-test`, which `app-test` runs from the
card. Two independently executed processes perform 64 round trips of a
4 KB buffer through each shared-memory API: POSIX queues synchronize one
exchange and System V semaphores the other. It also checks zeroed memory,
close-on-exec, unlink/removal while mapped, IPC statistics and time64,
three-argument and integer-argument `semctl`, message priority ordering,
nonblocking and timed receive, and information-buffer bounds. X's MIT-SHM
extension remains disabled; enabling and measuring it is separate work.
Two independently executed processes also perform 64 round trips using
process-shared pthread mutexes, condition variables and POSIX semaphores.

`libc` also builds `linux/artifacts/cxx-test`, which `app-test` runs from the
card after the thread test: exceptions through 40 frames with callee-saved
registers restored, through a 3 KB frame, rethrown, carried in an
`exception_ptr`, and thrown inside libstdc++; `bad_alloc`; RTTI and
cross-casts; static construction; iostreams, containers and `shared_ptr`;
and four threads throwing and catching concurrently under a mutex.

`rootfs` builds the root filesystem with Buildroot 2026.08 and leaves it as
`linux/artifacts/linux.img`, a 16 MB ext4 image with 4 KB blocks and no
metadata checksums. It goes on the card's FAT partition beside `linux.app`,
in one piece: every read into it walks its FAT chain, and on a card whose
free space is scattered the chain jumps about the FAT, which the device
measured at 1.3 s of every boot (6.05 s to the prompt against 4.76). A
partition freshly formatted with `newfs_msdos` and given `linux.img` first
keeps it contiguous, and a new image written over it in place keeps its
clusters (`dd if=linux.img of=/Volumes/WRBOOT/linux.img bs=1m
conv=notrunc`, where `cp` may reallocate it); `card-boot.py` boots wremu, through Grifo, on a `dd`
copy of a card's partition to see what it does.

The kernel's own initramfs holds only `initramfs/rootstart`, a 2 KB
freestanding program, and the device nodes it needs before devtmpfs. It
waits for the card and mounts it with `usefree`, trusting the card's own
free-cluster count; otherwise attaching the loop device has FAT read and
scan its whole allocation table. There is no RTC with a backup cell, so it
sets the clock from the newest of `linux.img`, `linux.app` and `kernel.elf`
before anything is mounted read-write. `linux.img` changes whenever the
system writes, so the clock carries on from the last session. It then attaches `linux.img` to `loop0`
with direct I/O, so ext4's reads neither start the FAT file's readahead nor
get cached twice. Every read into the image first finds its cluster by
walking the file's FAT chain, cached per file as at most eight runs of
consecutive clusters; the device's boot volume has 512-byte clusters, so
`linux.img` is a chain of 32,768. `patches/0020` follows runs of
consecutive clusters within a FAT block with a load and a compare a step
(through `fat_ent_read()` a step is about 1,300 cycles, and a walk down the
whole image 0.7 s), and reads ahead the FAT blocks a walk has still to
cross, up to the readahead window, in one request: a walk down the image
crosses 256 FAT blocks, each otherwise a request of its own, with the
card taking about 0.6 ms to its data token. It mounts the image `noatime`, credits the random seed BusyBox `seedrng` saved
last time, moves the card to `/mnt/sd`
inside it, mounts `/proc`, `/sys`, `/dev`, `/dev/pts`, and tmpfs on `/tmp` and
`/run` there (each a
BusyBox exec from `rcS`, about a tenth of a second; the tmpfs mounts keep
X's log, lock and socket out of the journal that `late` checkpoints, since
the next boot would replay them, 1.6 s on the device), and becomes its
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
and builds `wr-selftest`.
Buildroot builds with `c33-linux-uclibc-` from `toolchain`, as FDPIC with
shared libraries. Everything is compiled with `-ffunction-sections
-fdata-sections` and linked with `--gc-sections`, so a program keeps only
the functions it reaches, and a library only what it exports and they
reach. Buildroot does not notice a rebuilt C library, so every build
redoes every target package, about two minutes. The host tools (Meson,
Ninja, Python, bison and the rest) are built with the VM's own compiler and
depend on nothing of the C33's, so they are kept between builds;
`buildroot/build.sh --full` starts from nothing, about ten minutes, as a
change to a host package's options needs. The VM's CMake stands in for
Buildroot's, which took four and a half minutes to build for Ninja. A
package's `-dirclean` target and then `make -C` the output directory redoes
one package in a minute or two (`-rebuild` can leave a program unlinked
against its rebuilt libraries).

The image also carries `sl`, with ncurses and its terminfo. uClibc-ng
provides what Buildroot's own uClibc configuration offers packages, such as
the SUSv2 to SUSv4 legacy functions, `nftw`, GNU `glob`, `%m`, memory streams,
`wordexp` and `libutil`. It leaves out Sun RPC and `getcontext`, which has
no C33 implementation.

BusyBox 1.38.0 is an FDPIC program with an interactive `hush`, core file and
text tools, checksums, archive/compression tools, filesystem inspection, and
recovery utilities, and `hush` carries `busybox/patches/`. It is installed as
`/init`, `/bin/busybox`, and a symlink for every applet. BusyBox init
starts the consoles only once `rcS` ends, so `rcS` does almost nothing: on
this MMU-less machine every exec, and every background job (the shell
re-executes itself for one), is about a tenth of a second, and `hush`
parses a whole `if` block or function before running it. It checks for
the PTYs. With `wr.selftest` on the command line it runs
`/etc/init.d/selftest`: `wr-selftest`'s process, signal, trace and libc
checks, the display and input checks, and a Hush and
file/text/archive tool suite, then `linux.ok` and, later, the
`linuxhw.txt` device report on the card. The tests pass `wr.selftest` on the
launcher's `init.ini` line, and the kernel appends it on a direct boot, the
bring-up and recovery path. `init` finally respawns an interactive `hush` on
`ttyC0`.

Both consoles start login shells with `HOME=/root` and read `/etc/profile`.
Hush supports job control (`jobs`, `fg`, `bg`, Ctrl-Z), aliases, 64-bit
arithmetic, Ctrl-R history search, and a directory-aware prompt. History is
limited to 64 commands and saved to `/root/.hush_history` when the shell
exits normally; a forced reboot or power cut does not save that session.
`ll` and `la` abbreviate `ls -l` and `ls -la`. `EDITOR=vi`, `PAGER=less`, and
both consoles include `/mnt/sd/bin` in `PATH`. The home directory lives in
`linux.img`, so replacing that image replaces its history and personal files.

`vi` includes search, undo, yank/marks, repeat, colon commands, and settings.
`less` includes search, marks, line numbers, and horizontal scrolling, with
its input buffer limited to 4096 lines. File tools support recursive and
sorted listings, timestamps, human-readable sizes, NUL-delimited
`find`/`xargs`, symbolic-link inspection, temporary files, checksum checking,
gzip tarballs, unified diffs and `patch`. Process and memory inspection
includes `free`, `top`, `pstree`, `pgrep`, `pkill`, `pidof`, `pmap`, `lsof`,
`uptime`, `vmstat`, `iostat`, `watch`, and `sysctl`; `stty` inspects and changes
terminal settings. These are available commands, with no extra daemons
started at boot.

`wr-console` is the framebuffer frontend. It uses only standard
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
first button. `fbFillSpans` finds each
span's clip band by bisection (patch 0005) instead of walking every clip
box: a shaped window such as xeyes' has a box or two a row, and the walk
was most of the cost of filling its pupils. libX11 is built without its East Asian
multi-byte charsets (`buildroot/external/patches/xlib_libX11`), whose tables
were 410 KB of every X program, and libXfont2 with only its built-in fonts
(`buildroot/patches/0003`: no FreeType or font-file readers, 575 KB of the
server). With X up, 3.97 MB of text is mapped: Xfbdev 1.27 MB, twm 143 KB,
xeyes 30 KB, and 21 shared libraries, each loaded whole (libX11 696 KB, libc
489 KB, pixman 384 KB, libXt 305 KB). The static programs these replaced
kept only the functions they reached, about 3.3 MB in all: with only twm and
xeyes sharing libX11, sharing does not yet pay for the whole libraries.
Startup is mostly reading them from the card.

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
timer does. A transfer running late is looked at again after a block's
time, then twice that, up to a tick: a timer firing faster than it can be
handled starved every thread, the watchdog's worker included, and the
watchdog reset the machine while X loaded. The CPU follows behind, finding each token and
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
good. A reader waiting for its data yields the CPU to anything else
that is ready (`cond_resched()`) instead of spinning, since the DMA fills
the ring by itself: without preemption the spin kept every other task off
the CPU for the whole of a program's load, and the X server's own start-up
waited behind its clients'. Requests are up to 128 KB. A raw 4 MB read through Grifo takes 2.87
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
reads two and a half times slower. The slot's active-low chip select is
the tree's `cs-gpios`. The
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
Set `KERNEL` and `ROOTFS` to test alternate images without replacing the
default artifacts; `KEEP_LOG` retains the full boot log at the supplied path.
The same regression requires the SD host to announce streamed HSDMA block reads, the
DMA provider its polled channels, and the emulator to report nonzero HSDMA2 transmit and HSDMA3 receive
activity. It also
checks fbdev geometry, reads and rewrites the complete `/dev/fb0` image, and
requires the frontend to receive the scripted panel events from
`/dev/input/event0`.

## LTP POSIX tests

`make -C linux ltp-test` runs thirteen smoke cases. `make -C linux
ltp-all-test` attempts every numbered, non-speculative standalone interface
case in LTP's Open POSIX suite, then runs every successful build. Both use
unchanged upstream sources pinned to
`2279d708c817db657611a30c427c7d06c4360049` and the existing compiler,
kernel, and root filesystem. `make -C linux ltp-port-test` runs the local
regressions for bugs found during the sweep.

The first full sweep compiled 1,236 of 1,569 candidates: 262 needed
unavailable `fork`, and 71 needed unavailable `aio.h`. With a uniform
thirty-second guest deadline, 1,144 returned pass. The sweep exposed
signal-mask marshalling, time64 timer-output, clock capability, and
large-frame compiler bugs; a separate regression exposed the misaligned
C-helper call on `vfork` failure.

After fixes and appropriate deadlines, **1,183 cases pass individually**,
39 more than initially. Eight gains are deadline corrections; 31 exercise
the fixes or newly reported clock support. Seven local regressions and the
normal application checks pass too. The raw final sweep has 1,179 passes
and four execution errors caused by an emulator DMA stall after a tight
CPU-timer loop; all four affected tests pass in fresh boots. The combined
count is not a clean batch-sweep result. The contention-model issue is
documented separately and no speculative kernel storage fix is included.

Every test runs through FLASH -> Grifo -> launcher -> Linux, on an isolated
virtual card. The supervisor uses genuine `vfork` followed by `exec`,
records the program's exit status, and kills its process group on timeout.
Reports retain unsupported, untested, unresolved, failure, and timeout
results separately. The main LTP harness requires `fork`; replacing it
with `vfork` would change its semantics. These are standalone POSIX trials,
not a claim that all of LTP passes on no-MMU Linux.

See [ltp/README.md](ltp/README.md) for coverage, fixes, limitations, and
reproduction commands. Artifacts and detailed reports live under
`linux/artifacts/ltp-*`. These runs use wremu with 32 MiB SDRAM; physical
hardware and the 16 MiB configuration remain separate validation.

## Upstream BusyBox and uClibc tests

`linux/upstream-tests/` builds the upstream libc inventory and runs it and
BusyBox's applet and Hush suites through Grifo on disposable virtual cards.
It preserves build errors, runtime skips, failures, timeouts and crashes,
and checks individual shell coverage after an interrupted module. See
[upstream-tests/README.md](upstream-tests/README.md) for source revisions,
reproduction commands, results and the resulting bug backlog.

## What comes next

Richer keyboard modes, console session management, and power management can
grow around the proven LCD, touch, PTY, storage, and recovery userspace
paths, and an fbcon on the framebuffer would inherit the keyboard devices the
frontend now publishes.
