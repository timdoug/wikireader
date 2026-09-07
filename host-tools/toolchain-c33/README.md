# Modern GNU toolchain for Seiko Epson C33

This directory forward-ports EPSON's C33 support from binutils 2.10.1 and
GCC 3.3.2 to binutils 2.47 and GCC 16.2.

The target contract is [`gcc/ABI.md`](gcc/ABI.md). See the
[emulator guide](../../emulator/README.md) and
[ZIM reader guide](../../zim/README.md) for firmware use.

## Status

The installed `c33-epson-elf-*` toolchain builds and runs the complete
WikiReader firmware.

| Component | Current state |
| --- | --- |
| BFD and ELF | C33 objects, relocations, common sections, local-symbol merging, CTF, plugins, and all three core flags work. |
| gas | C33 Standard, Advanced, and PE assembly, `ext` prefixes, constants, relocations, and DWARF location views work. |
| ld | Links firmware and the upstream C33 suite; init/fini arrays, start/stop symbols, weak references, build IDs, and section GC work. |
| objdump/readelf/binutils | Read and disassemble shipped and newly built C33 ELF files. PE disassembly rejects instructions removed from the PE core. |
| GCC | GCC 16.2 C backend and three libgcc multilibs are complete for the currently supported ABI. |

The exact-source binutils testsuites have no unexpected failures:

| Suite | Results |
| --- | --- |
| gas | 338 passes, 10 unsupported |
| binutils | 240 passes, 18 untested, 17 unsupported |
| ld | 479 passes, 13 expected failures, 28 untested, 235 unsupported |

Unsupported cases are generic-suite features not supplied by this target;
they are not hidden C33 failures.

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

Firmware Makefiles default to the original EPSON compiler. Select GCC 16.2
explicitly:

```sh
make TOOLCHAIN_BIN="$(pwd)/host-tools/toolchain-c33/work/install/bin" <target>
```

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

- target triplet: `c33-epson-elf`;
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
