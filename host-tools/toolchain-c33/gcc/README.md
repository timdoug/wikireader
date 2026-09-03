# GCC 16.2 C33 backend

This directory contains the GCC 16.2 backend for the Seiko Epson C33 family.
The WikiReader uses the S1C33E07 PE core.

The implementation status shared with binutils and the emulator is summarized
in [`../HANDOFF.md`](../HANDOFF.md). The normative target description is
[`ABI.md`](ABI.md).

## Status

The backend and libgcc build as a freestanding `c33-epson-elf` C toolchain.
It builds every WikiReader stage, including the 8 KiB A0 boot applications,
kernel, `init.app`, and `wiki.app`. The resulting firmware boots through the
current full-system emulator and renders the tested screens identically to the
shipped GCC 3.3.2 firmware.

There is no known wrong-code failure in a supported C or ABI feature.

Implemented target facilities include:

- Standard, Advanced, and PE core selection with link-incompatible multilibs;
- register classes for the general registers, `%sp`, and address bases;
- C33 argument, return, aggregate, variadic, and stack-frame conventions;
- short and long direct calls, indirect calls, sibling calls, and returns;
- delayed branches and scheduling of the one non-annulling delay slot;
- exact 0/13/26-bit `ext` prefix selection and branch lengths;
- byte, halfword, word, stack-relative, absolute, `%r15`-relative, and
  post-increment memory forms;
- strict alignment and PE's mandatory address-error behavior;
- arithmetic, logic, shifts, rotates, multiply, bit operations, comparisons,
  branches, jump tables, software interrupts, and interrupt returns;
- generic libgcc integer division and soft-float helpers;
- stack trampolines and GCC's `__builtin_apply` / `__builtin_return`;
- ELF init/fini arrays, mergeable constants and strings, weak symbols, LTO,
  precompiled headers, DWARF 2, and CTF; and
- three installed libgcc multilibs, one for each C33 core variant.

The backend deliberately does not expose PE instructions that do not exist:
the older divide-step, MAC, mirror, scan, conditional-move, and V850-specific
operations are not generated.

## Build

Build binutils and the complete compiler into the common prefix:

```sh
# From the repository root.
host-tools/toolchain-c33/binutils/build.sh host-tools/toolchain-c33/work
host-tools/toolchain-c33/gcc/rebuild.sh
```

The compiler is:

```text
host-tools/toolchain-c33/work/install/bin/c33-epson-elf-gcc
```

`rebuild.sh` copies the maintained backend into a pristine GCC 16.2 source
tree, registers the target, applies the focused generic-GCC patches, builds
the compiler, and rebuilds all libgcc multilibs from scratch.

`build.sh` stops after `all-gcc` and exists for compiler bring-up. Do not use
it when validating firmware or ABI changes because it does not provide a
fresh installed libgcc.

Firmware Makefiles require explicit selection of this prefix:

```sh
make TOOLCHAIN_BIN="$(pwd)/host-tools/toolchain-c33/work/install/bin" <target>
```

Clean the affected firmware objects and libraries after changing compiler,
optimization level, ABI hooks, or target flags.

## Target options

| Option | Meaning |
| --- | --- |
| `-mc33`, `-mc33adv`, `-mc33pe` | Select the core and matching multilib. |
| `-mcore=...` | Equivalent core-selection spelling. |
| `-mno-long-calls` | Prefer direct short calls and jumps where range permits. |
| `-mlong-calls` | Use long direct call/jump forms. |
| `-medda32` | Use absolute data addressing. |
| `-memcpy` | Retained C33 target option. |

The firmware currently uses `-mc33pe -mno-long-calls -medda32 -O2`.
Boot stages use `-Os` and section garbage collection to fit A0. A current
full-FLASH A/B retains `-O2` for the runtime: `-Os` makes the installed
kernel/init/wiki files 9.1% smaller and retires 5.2% fewer instructions in
article retrieval, but is 3.2% slower under the current SDRAM/bus timing
model. See [`../HANDOFF.md`](../HANDOFF.md) for the complete measurements and
hardware-calibration caveat.

## ABI validation

The original GCC 3.3.2 compiler is an oracle for the binary interface, not for
optimized instruction sequences. `tests/abi/run-abi.sh` builds old/old,
new/new, old-callee/new-caller, and new-callee/old-caller programs. All four
combinations agree across five tested optimization levels.

The probes cover scalar and aggregate arguments, stack arguments, return
values, alignment, complex values, variadic calls, and forwarding through
`__builtin_apply`. Details and expected register layouts are in
[`ABI.md`](ABI.md).

## Correctness validation

- `gcc.c-torture/execute`: 24,260 passes, 251 legitimate unsupported
  results, zero failures or unresolved cases across all 1,692 sources and
  their standard option variants.
- `gcc.dg/torture`: exhaustively replayed with no GCC/backend failure.
- IPA: 807 passes, four expected failures, 13 external-prerequisite
  unsupported results, and no unexpected result.
- LTO: 1,651 passes, 34 unsupported external prerequisites, and no failures
  or unresolved cases.
- `gcc.dg/dg.exp`: the post-fix focused replay has 39,358 passes, four
  target-dependent scan/diagnostic mismatches, 534 expected failures, 1,037
  unsupported results, and no unresolved case.
- Differential tests compile the same 200 generated programs with GCC 3.3.2
  and GCC 16.2 at five optimization levels and compare execution with a native
  reference; all match.
- Complete modern firmware boots and renders the tested UI and article views
  byte-identically to the shipped firmware.
- The C33-specific target directory has 408 expected passes and four
  unsupported results. Its regression coverage includes rejection of indexed
  addresses for bit instructions, preventing recursive reload-pseudo growth.

The final unfiltered post-fix GCC run has not yet been performed. Use fresh
results from that run - not historical raw failures - as the next broad backlog.
See [`../tests/DEJAGNU-TODO.md`](../tests/DEJAGNU-TODO.md).

## Remaining work

### Validation

1. Run an unstripped C33 program under a real debugger and verify stepping,
   frames, arguments, variables, and unwinding.
2. Add independent runtime coverage for valid implemented PE operations not
   emitted by firmware or current differential programs.
3. Run the complete post-fix DejaGnu suite when the multi-hour qualification
   is desired.
4. Audit Darwin intentional-crash, SARIF, and diagnostic-path tests as host
   integration, without suppressing them.

### Optional compiler features

- `__int128`: define the ABI, alignment, argument/return behavior, TImode
  moves and arithmetic, and libgcc helpers.
- atomics: define interrupt, lock, and visibility semantics and enable
  libatomic; the target has no native compare-and-swap or thread model.
- heap trampolines: provide allocation and executable-memory runtime hooks.

These are new target features, not regressions in the supported ABI.

### External runtime boundary

Executable tests requiring libm, floating-point `printf`, hosted files,
process/environment/time/signal APIs, sanitizer runtimes, persistent gcov
output, constructor-array startup, semihosting, or threads remain external
runtime work. They must not be hidden as compiler passes or implemented in
firmware without separate approval.

## Source layout

```text
files/gcc/config/c33/          backend implementation and machine description
files/gcc/common/config/c33/   common option handling
files/libgcc/config/c33/       target libgcc configuration
patches/                       focused generic-GCC correctness patches
probes/                        ABI derivation sources
ABI.md                         target ABI and ISA contract
build.sh                       compiler-only bring-up build
rebuild.sh                     complete compiler and libgcc build/install
```

The backend started from GCC's maintained V850 structure because the original
EPSON C33 compiler was itself a V850 fork. C33 instruction selection, register
classes, ABI hooks, relocations, options, frames, and core distinctions are
implemented explicitly; V850-only behavior is not part of the target.
