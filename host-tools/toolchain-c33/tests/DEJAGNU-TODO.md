# C33 GCC/binutils test backlog

This file contains only current findings and future work. Historical run
totals are not a backlog: the next complete post-fix run is the authority.

## Scope

The active scope is correctness of the C33 GCC backend, C33 binutils, and the
DejaGnu integration required to execute and classify upstream tests.

Do not modify upstream tests, add target-specific skips for real failures, or
turn unknown link errors into unsupported results. Firmware, mini-libc, crt
startup, hosted target services, and new emulator facilities require separate
approval.

## Current qualification

| Suite | Current focused result |
| --- | --- |
| `gcc.c-torture/execute` | 24,260 passes, 251 legitimate unsupported, zero failures or unresolved cases across all 1,692 sources and standard variants |
| `gcc.dg/torture` | Exhaustively replayed; no GCC/backend failure remains |
| IPA | 807 passes, 4 XFAIL, 13 external-prerequisite unsupported, no unexpected result |
| LTO | 1,651 passes, 34 external-prerequisite unsupported, no failures or unresolved cases |
| `gcc.dg/dg.exp` | 39,358 passes, 4 target-dependent mismatches, 534 XFAIL, 1,037 unsupported, no unresolved cases |
| gas | 338 passes, 10 unsupported, no unexpected result |
| binutils | 240 passes, 18 untested, 17 unsupported, no unexpected result |
| ld | 479 passes, 13 XFAIL, 28 untested, 235 unsupported, no unexpected result |
| ABI cross-link | All old/new caller/callee combinations agree |

The complete unfiltered GCC suite has not been rerun since the focused fixes.
When it is run, preserve `gcc.sum` and `gcc.log`, group findings by source
and option set, and investigate only fresh unexpected results.

No compact non-sanitizer execution family from the previous full run remains
unclassified. Long standard tests are supported by the deterministic
3.2-billion-instruction board ceiling and GCC's normal per-test timeout
factor.

## Visible compiler-only mismatches

These results stay visible. Current evidence does not show incorrect C33 code
or malformed debug information:

- `ifcvt-4.c`: expects multi-set conditional conversion. C33 has no
  conditional-move instruction and correctly retains a branch.
- `pr87954.c`: performs the widened multiply once; the C33 dump spells it
  `w*`, outside the generic scan's accepted spellings.
- `stack-usage-1.c`: reports 272 bytes - the requested 256-byte object plus
  C33's 16-byte `%r0`-`%r3` save block. The generic accepted-size list
  omits this ABI.
- `debug/dwarf2/inline5.c`: the emitted graph contains one abstract
  variable, one inlined instance, and one out-of-line instance. The scan
  counts comments because one character class omits C33's `;` comment
  marker; `readelf` confirms the graph is valid.
- `pr126464.c`: executes successfully. Its additional overflow warnings are
  correct because C33 `long double` is ABI-valid binary64 and `1e4000L`
  overflows it.

`ipa-icf-12.c` and `ipa-icf-13.c` can also report one extra valid identical
pair: C33 `int` and `long` are both 32 bits, so mini-libc's `abs` and
`labs` bodies are identical. The requested folds still occur.

Revisit these only if new inspection demonstrates wrong code, an ABI error,
or malformed debug information. Do not edit or skip the upstream source to
change the count.

## Target feature opportunities

These are explicit implementation projects, not excuses to weaken target
feature probes:

### `__int128`

Define the C33 representation, alignment, argument/return convention, TImode
moves and arithmetic lowering, and required libgcc helpers. Most 32-bit GCC
targets do not expose this type, so its absence is not a regression in the
existing C33 ABI.

### Atomics

C33 has no native compare-and-swap and this toolchain has no thread model.
Supporting the atomic suites requires a defined interrupt/lock/visibility
model and an enabled libatomic. The compiler is currently configured with
`--disable-libatomic`.

### Heap trampolines

Ordinary stack trampolines work and `gcc.dg/trampoline-1.c` executes.
GCC's separate heap-trampoline method needs allocator, lifetime, and
executable-memory runtime hooks.

## Toolchain validation opportunities

- Exercise an unstripped C33 executable with a real debugger. Line tables and
  ordinary DWARF info are valid, but stepping, unwinding, variables, and
  frames have not been qualified end to end.
- Audit intentional compiler-crash, SARIF, and diagnostic-path behavior on
  Darwin as host GCC integration.
- Add independent runtime coverage for implemented PE operations not emitted
  by firmware or current differential programs, especially stack-special and
  indirect-jump forms.
- Run the final complete post-fix GCC suite.

## External target-runtime prerequisites

These are accurately reported as unsupported only when the failed test names
no unrelated missing symbol:

- C99 libm and floating-point `printf`;
- hosted file, environment, time, signal, and process APIs;
- persistent `.gcda` output and `__gcov_exit` transport;
- sanitizer runtimes and their language-runtime dependencies;
- crt iteration of emitted constructor/destructor arrays; and
- semihosting, persistent host files, or a thread/TLS runtime.

`libgcov.a` is built and installed, including counter and merge machinery;
the freestanding target lacks the termination writer and persistent
transport. GCC emits and ld retains correctly ordered init/fini arrays; only
crt iteration is missing. Single-thread emulated TLS works; a native
multi-thread TLS ABI is a separate feature.

Do not add stubs solely to turn these results green. Ask before implementing
anything in this section.

## Triage rules

For every fresh unexpected result, distinguish:

1. wrong generated code or ABI behavior;
2. assembler, linker, or object-format behavior;
3. incorrect compiler diagnostics;
4. DejaGnu execution or classification failure;
5. a documented external runtime prerequisite; or
6. a target-dependent scan expectation.

An implementation item is complete only when its cause is understood, a
focused upstream test passes without source modification or a skip, relevant
core/call/multilib variants are checked, and the next broad run shows no
regression.
