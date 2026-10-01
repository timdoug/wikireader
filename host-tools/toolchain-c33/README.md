# Modern GNU toolchain for Seiko Epson C33

This directory forward-ports EPSON's C33 support from binutils 2.10.1 and
GCC 3.3.2 to binutils 2.47 and GCC 16.2.

The target contract is [`gcc/ABI.md`](gcc/ABI.md). See the
[emulator guide](../../emulator/README.md) and
[ZIM reader guide](../../zim/README.md) for firmware use.

The [C33 ABI v2 proposal](gcc/ABI-v2-proposal.md) describes a possible
replacement for bare-metal applications, NuttX and Linux, including factory
boot compatibility and the measurements required before adopting it.
It is deferred while we bank ABI-compatible improvements; the current target
contract remains `gcc/ABI.md`.

## Status

The installed `c33-epson-elf-*` toolchain builds and runs the complete
WikiReader firmware.

| Component | Current state |
| --- | --- |
| BFD and ELF | C33 objects, relocations, common sections, local-symbol merging, CTF, plugins, and all three core flags work. |
| gas | C33 Standard, Advanced, and PE assembly, `ext` prefixes, constants, relocations, and DWARF location views work. |
| ld | Links firmware and the upstream C33 suite; init/fini arrays, start/stop symbols, weak references, build IDs, and section GC work. Links FDPIC executables and shared libraries for Linux. |
| objdump/readelf/binutils | Read and disassemble shipped and newly built C33 ELF files. PE disassembly rejects instructions removed from the PE core and words that violate fixed opcode bits. |
| GCC | GCC 16.2 C backend and three libgcc multilibs are complete for the currently supported ABI. |

The exact-source binutils testsuites have no unexpected failures:

| Suite | Results |
| --- | --- |
| gas | 339 passes, 10 unsupported |
| binutils | 240 passes, 18 untested, 17 unsupported |
| ld | 479 passes, 13 expected failures, 28 untested, 235 unsupported |

Unsupported cases are generic-suite features not supplied by this target;
they are not hidden C33 failures.

## Improvements within the current ABI

Preserved registers are allocated in ascending order, `%r0` first. Functions
retain the established contiguous `pushn`/`popn` saves through the highest
used preserved register, on all core types. Prefer low registers to reduce
unnecessary saves without adding another frame-lowering implementation.
The argument, result, data-pointer and register-preservation contracts remain
the same, including sixteen-byte call alignment. Rebuilding a component
captures these compiler improvements without migrating its interfaces.

Linux signal entry now emulates a normal call's return-address push:
handlers enter at `SP % 16 == 12`. Assembly entry probes check this before
the C prologue on ordinary and nested alternate stacks. The signal context
layout stays the same.
The kernel's stack-overflow check also uses the active alternate stack's own
lower bound. The newly rebuilt kernel exposed its previous assumption that
every userspace SP belonged to the ordinary stack; the same regression now
passes with the corrected guard and selected compiler.

The selected compiler passes 489 target checks and 50 native-versus-target
generated programs. All 20 old/new ABI cross-call output streams across
`-O0`, `-O1`, `-O2`, `-O3` and `-Os` match the preserved pre-change compiler.
That baseline already has a legacy-caller/modern-varargs mismatch; this
comparison does not claim full historical compatibility. The focused frame
tests likewise retain exactly the baseline's 214 passes and 22 failures in
three nested-function cases. These remain independent correctness issues.
Current logs, rebuilt images and hashes are in `work/abi-simple/`, with the
authoritative target summary in `target-final/gcc.sum` there.

Doom and ZIM have rebuilt with the selected compiler; Doom passes the full
FLASH-to-Grifo-to-launcher benchmark and log-persistence check. The rebuilt
Linux compilers, runtimes, kernel and root filesystem pass the complete
launcher, signal-entry, nested alternate-stack, NPTL/TLS, pthread, atomics,
IPC, C++ exception/cancellation and suspend regression. The unwind library
forces the highest preserved register into its contiguous save block.
These are emulator correctness checks; the selected implementation has not
been timed on physical hardware.

The initial 2026-09-30 experiment combined this allocation-order change
with sparse individual saves on ADV/PE. That additional save implementation
was subsequently removed in favor of the established block-save and unwind
code: correctness and simplicity take priority, and the physical Doom test
did not demonstrate a speedup. The table compares the original baseline,
that discarded experiment, and the selected allocation-only implementation.
All three rebuilt the same ZIM and Doom sources and flags, holding the
existing Grifo and mini-libc archives fixed. Timings are single emulator
runs; the other rows are static image counts. The selected build's modeled
FPS fell by 0.61%, so fewer static saves do not establish a performance win.

| Measurement | Original baseline | Discarded sparse saves | Selected contiguous saves |
| --- | ---: | ---: | ---: |
| Doom controlled E1M1 benchmark | 15.679 FPS | 15.775 FPS (+0.61%) | 15.583 FPS (-0.61%) |
| Doom engine time per frame | 55.474 ms | 55.091 ms | 55.867 ms |
| ZIM words summed across static push sites | 1,860 | 1,517 | 1,517 |
| Doom words summed across static push sites | 1,353 | 995 | 997 |
| ZIM text size (`size`) | 318,525 bytes | 318,497 bytes | 318,497 bytes |
| Doom text size (`size`) | 245,129 bytes | 245,033 bytes | 245,017 bytes |

The physical WikiReader test used the baseline and discarded sparse-save Doom binaries with
the same existing Grifo, launcher and WAD. One completed ten-second run per
build measured 16.069 FPS before and 16.063 FPS after (-0.038%); engine time
was 53.548 ms versus 53.572 ms per frame. Both runs had identical scene,
clock and SDRAM settings, no input or scene changes, and no measurement-window
card I/O or errors. This pair shows essentially unchanged hardware performance;
it does not establish either a speedup or a regression at this scale. Repeated
runs would be needed to assess such a small difference. The raw log and parsed
results are archived in `build/abi-compatible-card/hardware-859168e77f71/`.

The push-site counts sum `N + 1` for each disassembled `pushn rN` and one
for each `push rN`, including handwritten saves and unchanged libraries.
Count each decoded instruction once, ignoring the repeated mnemonic in
objdump's annotation column.
They are static counts, not execution frequencies, stack high-water marks
or application speedups. ZIM has been rebuilt; its runtime performance
has not been measured in this comparison.

Before/after compilers, binaries, disassembly and verification logs are
retained under `work/abi-compatible/`. Controlled Doom logs and exact image
hashes are in `build/doom/trace-t7_kdxi9` (before) and
`build/doom/trace-sow58sc6` (discarded experiment). The selected build's
matching log and hashes are in `build/doom/trace-qj05g8z4`.
Reproduce the controlled comparison with
`doom/trace-benchmark.py WAD --app IMAGE --map MAP`.

## Build and install

Use one prefix for binutils, GCC, and libgcc:

```sh
# From the repository root.
host-tools/toolchain-c33/binutils/build.sh host-tools/toolchain-c33/work
host-tools/toolchain-c33/gcc/rebuild.sh
```

The result is installed under:

```text
host-tools/toolchain-c33/work/install/
```

`gcc/rebuild.sh` is preferred over the bring-up-only `gcc/build.sh`: it
builds and installs the compiler and forcibly refreshes every libgcc multilib.

Firmware Makefiles use this installation by default, so `make <target>` needs
no toolchain argument. Set `TOOLCHAIN_BIN` to select a different one, such as
`host-tools/toolchain-install/bin` for the original EPSON compiler.

Clean the relevant firmware component when switching toolchains or ABI flags;
Make does not encode compiler identity or flag changes in object-file
dependencies. For a complete firmware rebuild, build
`samo-lib/{mini-libc,fatfs,drivers,grifo}`, then `wiki` and `zim` in that
order. Rebuild Grifo before running host reader tests: cleaning it removes
the generated `grifo.h`. The emulator's `samo-lib/mbr/flash.rom` build needs
gawk; physical devices retain their factory flash.

## Validation against the original toolchain

The original toolchain installs to `host-tools/toolchain-install/`:

```sh
make toolchain
```

It remains an assembler and ABI oracle. The modern assembler comparison covers:

- 86 hand-written firmware assembly sources: identical `.text`, `.data`,
  relocations, `e_machine`, and C33 core flags;
- 143 sources emitted by GCC 3.3.2: identical `.text`; and
- 20 local-relocation spelling differences that link to identical code.

Run it with:

```sh
host-tools/toolchain-c33/tools/compare-with-oracle.sh
```

Compare sections and linked results, not complete object files: modern and
legacy ELF containers legitimately differ in section and symbol-table layout.

Additional independent checks are:

- cross-linked old/new ABI probes in `tests/abi/`;
- GCC's standard DejaGnu drivers in `tests/dejagnu/`;
- generated native-versus-C33 execution in `emulator/difftest/`; and
- full firmware boot and UI comparison in `emulator/`.

## Supported target contract

- target triplets: `c33-epson-elf` for bare metal, and `c33-linux-uclibc`
  for no-MMU Linux userspace, FDPIC (below);
- cores: `-mc33`, `-mc33adv`, and `-mc33pe`;
- PE is the WikiReader core and uses strict natural alignment;
- `-mno-long-calls` selects direct short calls when range permits;
- `-medda32` selects absolute data addressing; the alternative is C33
  `%r15`-relative data addressing;
- comments use `;`, symbols have no leading underscore, and ELF uses
  `EM_SE_C33` (107);
- core type is recorded in ELF `e_flags`, and incompatible core objects do
  not link;
- PE software division comes from generic C libgcc helpers because the PE core
  removes the older divide-step instructions.

`c33-linux-uclibc` is the same backend configured by `gcc/config/c33/linux.h`,
building FDPIC ELF for no-MMU Linux with shared libraries: `-mcore=c33pe
-mfdpic` and short calls by default, with the FDPIC rules in
[`gcc/ABI.md`](gcc/ABI.md). A direct call never leaves its module (a call
to another goes through the module's `.plt`), and a short call reaches 2 MB
either way; `ld` reports a call out of reach, and a bigger module needs
`-mlong-calls`. It defines `__linux__`, `__unix__`, `__uClinux__`
and `__FDPIC__`, and uses glibc's `<stdint.h>` type conventions. Its `ld`
defaults to the `c33fdpic` emulation (`elf32-c33fdpic` output), which writes
`.plt` entries, function descriptors, dynamic relocations and the `.rofixup`
list executables relocate themselves with; a program links against
uClibc-ng's `libc.so` and runs under its `ld-uClibc.so`. `binutils/build.sh`
and `gcc/rebuild.sh` build it with `C33_TARGET=c33-linux-uclibc`;
`linux/toolchain.sh` supplies the sysroot.

See [`gcc/ABI.md`](gcc/ABI.md) for registers, frames, arguments, returns,
variadic forwarding, relocations, instruction extension, and exception rules.

## Layout

```text
binutils/build.sh           pristine binutils 2.47 build/install
binutils/files/             C33 BFD, gas, ld, opcodes, and testsuite sources
gcc/rebuild.sh              complete GCC 16.2 and libgcc build/install
gcc/files/                  C33 GCC backend sources
gcc/patches/                focused upstream GCC correctness patches
gcc/ABI.md                  target ABI and ISA contract
gcc/README.md               GCC-specific status and validation
tests/abi/                  old/new cross-link tests
tests/dejagnu/              standard GCC board and runner
tests/DEJAGNU-TODO.md       current test and feature backlog
tools/glue.py               registers C33 in a pristine binutils tree
tools/gcc-glue.py           registers C33 in a pristine GCC tree
tools/compare-with-oracle.sh assembler comparison
```

Build trees live under `host-tools/toolchain-c33/work/` and are disposable.
The maintained target sources live under `binutils/files/`, `gcc/files/`,
and `gcc/patches/`.
