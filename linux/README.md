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

Linux initializes 32 MiB of SDRAM, registers the S1C33 interrupt controller
with Linux's generic IRQ subsystem, and starts a 100 Hz timer. The timer,
both UARTs, and SPI receive-DMA paths use normal `request_irq()` registrations
visible in `/proc/interrupts`. Linux runs the scheduler, registers the
interrupt-driven `ttyC0` UART console, and runs static BusyBox 1.38 as PID 1.
BusyBox init supervises an interactive Hush recovery shell on `ttyC0` and a
separate framebuffer console on a Unix98 PTY.

Early userspace also runs a process-lifecycle regression. It
uses the asm-generic `clone(CLONE_VM | CLONE_VFORK)` ABI, executes a second
bFLT image as `/child`, and reaps its exit status with `wait4`. This
exercises the live syscall register frame, task creation, scheduling, exec,
exit, and parent wakeup without relying on a C library.

The same regression installs a `SIGUSR1` handler with `rt_sigaction`, delivers
the signal to PID 1, and returns through the C33 `rt_sigreturn` trampoline. The
kernel saves and restores the complete integer context, signal mask, and
alternate-stack state in an aligned `ucontext` frame on the userspace stack.

PID 1 then executes a static uClibc-ng bFLT program. The program enters through
the C33 CRT, calls `printf()` and `getpid()`, verifies `setjmp()`/`longjmp()`,
exits through libc, and is reaped by PID 1. This is the first regression using
the conventional C userspace ABI rather than the initramfs syscall veneers.

The architecture's small early renderer writes `C33 LINUX` into the LCD
memory left active by the card loader, then becomes a temporary printk console
that scrolls the ordinary kernel log until late init. Boot checkpoints remain
visible below the text and an unhandled exception replaces their strip with a
solid fault bar, so failures before userspace remain visible without attaching
to the serial pads. The boot logo is placed on the physical right edge.
Once init is running, `/sbin/wr-console` takes over the ordinary fbdev device.
It renders a 40-column terminal and soft keyboard, allocates a Unix98 PTY from
`/dev/ptmx`, makes the PTY slave Hush's controlling terminal, and translates
released soft keys into terminal input. Its four-row keyboard provides
lowercase and shifted letters, `123`/`ABC` symbol pages, Control, Tab, Space,
cursor keys, Backspace, and Enter. The frontend writes only changed text rows
and key bands through fbdev. Packed glyph writes and overlap-safe framebuffer
row moves make scrolling cheap; bounded output frames are paced when they
scroll so commands remain visibly animated instead of either repainting every
byte or jumping directly to their final screen. BusyBox init respawns the
frontend if it exits; the
independent `ttyC0` recovery shell remains available throughout.

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
The same 240x208 one-bit memory is registered with fbdev as `/dev/fb0` for
ordinary applications. The early renderer remains independent of fbdev so it
can still report failures before platform drivers have probed. The kernel's
standard monochrome Tux asset is enabled and briefly drawn when fbdev probes;
the userspace console replaces it after init completes.

The card ROM and MBR load `kernel.elf` with the S1C33E07 still running from
its 48 MHz OSC3 reset clock; the 60 MHz PLL setup normally belongs to Grifo,
which this boot path replaces. Linux decodes the live CMU clock selection and
publishes MCLK through the common clock framework. Both serial ports and the
SPI controller acquire and enable that standard clock; their drivers no longer
call a board-specific clock callback or receive a copied clock rate. The early
tick timer uses the same hardware decoder before clock providers are available.
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
```

`fetch` reconstructs the pinned upstream kernel and uClibc-ng revisions from
`revisions` on the VM disk, applies their patches, then installs their
overlays. Each kernel build also refreshes `overlay/` in the VM source tree so
iterative port changes cannot be silently missed. It leaves the final kernel
in `linux/artifacts/`.

`libc` builds and installs a static, no-MMU C33 uClibc-ng with the native
asm-generic syscall ABI and time64 interfaces. Its link regression compiles a
real `stdio.h` program, resolves it with the C33 PE `libgcc`, verifies that the
ELF has no undefined symbols, and converts it to a Linux-loadable bFLT image at
`linux/artifacts/uclibc-smoke`. The regular `build` target depends on this
image and embeds it in the initramfs as `/uclibc-smoke`.

`busybox` builds the pinned BusyBox 1.38.0 release as a static C33 bFLT. Its 77
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
GPIO descriptor. Neither the SPI driver nor its platform data contains a
board-specific chip-select callback.
It also sends aligned, all-ones bulk reads through the S1C33 HSDMA2/HSDMA3
transmit/receive pair. Short, unaligned, command, and write transfers retain a
bounded programmed-I/O path, so the optimization remains entirely behind the
standard SPI controller API. Bulk reads sleep on a Linux completion signaled by
the HSDMA3 IRQ, with a bounded latched-cause check only for lost-interrupt
recovery. The kernel includes FAT/VFAT and mounts the first
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

The next normalization slice is replacing the remaining SPI clock-pin hold
and MMC power callbacks with pin-control and regulator consumers backed by the
new GPIO provider, then separating the embedded HSDMA implementation behind
DMAengine. That will let SPI/MMC consume the same resources as device-tree
systems.
Richer keyboard modes, console session management, and power management can
then grow around the proven LCD, touch, PTY, storage, and recovery userspace
paths.
