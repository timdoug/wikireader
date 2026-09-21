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
UART, touch, and SPI receive-DMA paths use normal `request_irq()` registrations
visible in `/proc/interrupts`. Linux runs the scheduler, registers the
interrupt-driven `ttyC330` UART console, and loads a tiny compiled-C bFLT
process as PID 1.
PID 1 provides an interactive
`h`/`p`/`c` shell: it reads commands through the Linux TTY layer, invokes
`getpid()` for `p`, reports its global command counter for `c`, and remains
alive while timer interrupts keep preempting native C33 userspace.

Before entering the shell, PID 1 also runs a process-lifecycle regression. It
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

The UART console is mirrored to a 30-column text console in the LCD framebuffer
left active by the card loader. A fixed strip below the text records memory,
interrupt, timer, UART, userspace, and UART-RX checkpoints even as the text
scrolls. Touch adds four more boxes for interrupt, packet start, complete
packet, and injected key; a twelfth box means a UART1 receive error was seen.
An unhandled exception replaces the strip with a solid fault bar. This makes
real-hardware boot results visible without attaching to the serial pads.

The card ROM and MBR load `kernel.elf` with the S1C33E07 still running from
its 48 MHz OSC3 reset clock; the 60 MHz PLL setup normally belongs to Grifo,
which this boot path replaces. Linux decodes the live CMU clock selection and
uses that rate for the tick timer and both UART divisors. This also keeps a
kernel entered by already-running firmware correct if that firmware selected
the PLL first.

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
`init` finally respawns an interactive `hush` on `ttyC330`. `/diag-init`
remains available as the old freestanding rescue shell.

The native S1C33 SPI controller driver and Linux's generic `mmc_spi` stack
power and pin-mux the WikiReader card slot, identify SDSC and SDHC cards, and
expose standard devices such as `/dev/mmcblk0p1`. The controller presents
ordinary 8-bit full-duplex SPI semantics, but batches bulk transfers into
32-bit hardware characters while preserving their wire byte order. It handles
all four SPI modes and the hardware's MCLK/4 through MCLK/512 divisors. It
reprograms the clock before chip select is asserted because disabling the
S1C33 serial block while it drives SCLK creates a real stray edge. The kernel
also sends aligned, all-ones bulk reads through the S1C33 HSDMA2/HSDMA3
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
and verifies its output and vector 57 interrupt. It separately generates panel
taps for a command and Enter key, requires the UART1 touch interrupt to feed
that command through the on-screen keyboard into the same shell, and checks
the resulting output. The test also checks the final display image for console
text, all seven LCD checkpoints, and the three keyboard rows. The fixture and emulator
display output are kept outside the checkout and removed afterward. The card
fixture is writable only for this isolated run; after the guest exits, the
host parses its raw FAT image and requires `linux.ok` to contain the expected
status. A console claim without persisted card bytes therefore fails the test.
The same regression requires the Linux driver to announce IRQ-driven HSDMA,
requires vector 25 to have a nonzero `/proc/interrupts` count, and requires the
emulator to report nonzero HSDMA2 transmit and HSDMA3 receive activity.

## What comes next

The next useful vertical slices are a framebuffer interface and fuller device
description in standard kernel data structures. Richer keyboard modes and
power management can then grow around the proven LCD, touch, console, storage,
and recovery userspace paths.
