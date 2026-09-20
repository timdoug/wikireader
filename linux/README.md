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

PID 1 is ordinary linked C apart from its entry point and four syscall veneers.
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
make -C linux build
make -C linux boot-test
```

`fetch` reconstructs the pinned upstream kernel revision from `revisions` on
the VM disk, applies `patches/`, then installs `overlay/`. Each `build` also
refreshes `overlay/` in the VM source tree so iterative port changes cannot be
silently missed. It leaves the final kernel in `linux/artifacts/`.

`boot-test` runs on macOS. It creates an isolated temporary FLASH/FAT32
fixture, boots it through the full emulated hardware path, injects two bytes into
UART0 after PID 1 starts, and passes only if vector 57 fires, userspace reads
the `p` and `c` commands, and the shell prints `pid 1` and `commands 2` without
a kernel panic. The fixture and emulator display output are kept outside the
checkout and removed afterward.

## What comes next

The next useful vertical slice is a small C library and line-oriented shell.
Signal delivery and `rt_sigreturn` also need validation before larger
applications. After that come SD/block/filesystem support and the WikiReader
panel, input, and power drivers.
