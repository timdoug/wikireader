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

Linux initializes 32 MiB of SDRAM, the interrupt controller and 100 Hz timer,
runs the scheduler, registers the interrupt-driven `ttyC330` UART console, and
loads a tiny compiled-C bFLT process as PID 1. PID 1 provides an interactive
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

The UART console is mirrored to a 30-column text console in the LCD framebuffer
left active by the card loader. A fixed strip below the text records memory,
interrupt, timer, UART, userspace, and UART-RX checkpoints even as the text
scrolls. An unhandled exception replaces it with a solid fault bar. This makes
real-hardware boot results visible without attaching to the serial pads.

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
`linux/artifacts/uclibc-smoke`.

`boot-test` runs on macOS. It creates an isolated temporary FLASH/FAT32
fixture, boots it through the full emulated hardware path, injects two bytes into
UART0 after PID 1 starts, and passes only if vector 57 fires, userspace reads
the `p` and `c` commands, and the shell prints `pid 1` and `commands 2` without
a kernel panic. It additionally requires `/child` to run and PID 1 to reap its
exit status. The test checks the final display image for text and all seven LCD
checkpoints. The fixture and emulator display output are kept outside the
checkout and removed afterward.

## What comes next

The next useful vertical slice is running the uClibc smoke binary under the
kernel, followed by a minimal static BusyBox configuration. After that come
SD/block/filesystem support and the WikiReader panel, input, and power drivers.
