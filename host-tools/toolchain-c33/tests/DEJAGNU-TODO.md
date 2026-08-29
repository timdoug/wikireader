# C33 GCC/binutils DejaGnu backlog

This is the implementation backlog exposed by the first unfiltered GCC 16.2
DejaGnu run. The historical baseline is
`dejagnu/work/full-20260828-2130/gcc.sum`: 145,212 expected passes and 1,836
unexpected failures. It predates all fixes below and is not the current
status. Raw assertion counts are useful for measuring progress, but one source
repeated across option sets is normally one root cause. A new unfiltered
baseline is deliberately deferred until the focused failure families are
finished.

## Scope

The current task is correctness of the C33 GCC backend and C33 binutils.
Changes needed only to execute and classify the upstream tests in DejaGnu are
also in scope. Do not implement firmware, mini-libc, crt startup, target
services, or emulator features without asking first.

Tests blocked by one of those external prerequisites remain visible below.
They are not GCC/binutils implementation work, and they must not be made to
pass by changing an upstream test or by adding a target-specific skip.

## P0: GCC backend and ABI correctness

### Inline string operations: fixed and qualified

`gcc.dg/torture/inline-mem-cmp-1.c` passes all 21 focused verdicts, and
`inline-mem-cpy-cmp-1.c` passes all 28.  The latter's `-O0` execution needs
3,433,401,134 emulator instructions.  It used to stop at the board's
3.2-billion base budget even though the upstream source requests
`dg-timeout-factor 2`: target loading incorrectly consulted Expect's raw
`timeout` variable, which does not include GCC's per-test factor.  The board
now uses GCC's standard `timeout_value` helper to scale both the host watchdog
and deterministic instruction budget.  No upstream source was changed.

```sh
./run-dejagnu.sh all dg-torture.exp=inline-mem-cmp-1.c
./run-dejagnu.sh all dg-torture.exp=inline-mem-cpy-cmp-1.c
```

### Remaining execution regressions

No compact non-sanitizer execution family from the baseline remains
unclassified. Continue
from fresh failures in the next broad run rather than assuming every old raw
failure is still reproducible.

For each source, first separate wrong code, an assembler/linker defect, a
compiler diagnostic defect, a test-execution problem, and an external runtime
prerequisite. Do not add ignore entries.

Already classified baseline executions are not backend defects:

* `pr112344.c` was forced into upstream's expensive mode by the wrapper and
  exceeds the simulator budget outside `-O2`/`-O3`; the wrapper no longer
  enables expensive tests by default.
* `pr97459-{1,2,3,4,5,6}.c` produce correct arithmetic. With the current
  compiler, the slowest standard variant (`pr97459-6.c`) needs 3,033,306,485
  emulator instructions, so the deterministic board ceiling is 3.2 billion
  rather than the original one billion.
* `pr47917.c`, `pr79800.c`, `builtin-sprintf.c`, `strlenopt-68.c`,
  `pr78965.c`, and `ipa/pr96040.c` exercise missing `snprintf`/floating-format
  runtime behavior.
* `pr118224.c` exercises allocator failure and overflow semantics supplied by
  the test runtime.
* `ipa/pr70306.c`, `constructor-1.c`, and `initpri1*.c` need crt startup to
  execute emitted constructor/destructor arrays.
* `loop-interchange-1.c` and `loop-interchange-1b.c` allocate about 14 MiB,
  beyond the current 8 MiB test memory map.
* `torture/matrix-{3,4}.c` include the hosted `<math.h>` even though they are
  compile-only matrix-flattening tests; the freestanding library has no such
  header.  Source prerequisite checks now apply to compile-only tests too, so
  all six `matrix-*` sources produce 48 explicit unsupported verdicts rather
  than two sources failing compilation.
* `torture/builtin-convert-2.c` uses `builtins-config.h`'s hosted-runtime
  heuristic to enable C99 math calls, then requires the absent libm. The
  board's `c99_runtime` result cannot affect that source-level `#ifdef`.
* `torture/pr59330.c` deliberately defines `free`.  The DejaGnu-only runtime's
  allocator fallback is now weak, as board support must allow a program under
  test to override it.  The focused upstream matrix passes all 14 compile and
  execution verdicts; neither the firmware runtime nor the upstream test was
  changed.

The post-fix `gcc.dg/torture` driver has been exhaustively replayed in
path-qualified partitions.  No GCC/backend failure remains.  The sole visible
failure is `pr47917.c -O0`, which reaches mini-libc's non-conforming `snprintf`;
all optimized and LTO variants pass because GCC performs the expected folds.
It remains visible rather than being hidden behind a test-name exception.

The IPA driver is likewise fully classified.  Its expected folds and
executions pass except for four external assertions: `ipa-icf-{12,13}.c` find
all requested equalities plus the genuine additional equality between
mini-libc's header-defined `abs` and `labs` (both are 32-bit on C33), while
`pr70306.c` needs crt constructor iteration and `pr96040.c` needs conforming
formatted output.  None is a missed IPA optimization or backend error.

The current replay of the old top-level `gcc.dg` failures has 37 failing
source names (61 assertions), down from 48 sources/72 assertions before the
latest fixes. They split as follows:

* missing headers/library surface: `alias-11`, the three atomic multi-TU
  tests, C90/C99 header tests, float-range/math tests, `spellcheck-inttypes`,
  `sso-14`, and `struct-ret-libc`;
* missing libm/C99 math implementation: `builtins-58`, `builtins-67`,
  `fold-round-1`, `pr120638`, `pr36584`, `pr41963`, and related math scans;
* external execution services: hosted libgcov output (`20020201-1`), crt
  constructors (`constructor-1`), allocator semantics (`pr118224`), and
  formatted/string runtime behavior (`strlenopt-68`);
* target-insensitive optimization scans with correct generated behavior:
  `ifcvt-4`, `pr87954`, and `stack-usage-1`; and
* no remaining GCC/backend item in this replay; the former `builtin-apply2`
  and stack-alignment failures are fixed by the forwarding ABI extension
  recorded below.

`dg-output-file-1` is no longer in that list: successful simulator loads now
return only the target UART stream to DejaGnu, while retaining the full
emulator transcript for failures.

## P1: binutils and target-format correctness

### Thread-local storage

GCC's existing single-thread `emutls` path works. The ten old
`gcc.dg/debug/tls-1.c` failures came from static archive order: libc was
scanned before `libgcc` introduced its `memcpy` reference. The DejaGnu board
now repeats libc after libgcc, like GCC's normal specs, and all ten variants
pass. A native multi-thread C33 TLS ABI, relocations, thread-pointer setup and
an emulator thread model would be a separate feature, not a requirement for
the current compiler tests.

### Constructors: compiler/linker portion

Fixed. GCC is configured with `--enable-initfini-array`, including rebuilds of
an existing work tree, and emits typed `.init_array.N`/`.fini_array.N`
sections. The updated linker retains and consolidates them, defines hidden
`__init_array_{start,end}` and `__fini_array_{start,end}` boundaries, and
orders priority 100 before priority 200 in a focused link probe. GCC's
`constructor-1.c` now compiles and links; its execution still fails because
the test crt does not iterate those arrays. Adding that startup behavior is
external runtime work and is not hidden by a skip.

### Full binutils qualification

The current exact-source binutils suites have no unexpected failures:

* gas: 320 passes and 10 unsupported tests;
* binutils utilities: 240 passes, 18 untested, and 17 unsupported tests; and
* ld: 479 passes, 13 expected failures, 28 untested, and 235 unsupported
  tests.

These totals include the C33 assembler, BFD, readelf, linker-script,
start/stop-symbol, init-array, build-id, section-discard, and local-relocation
fixes. The tested binutils are installed into the active GCC prefix.

Undefined-symbol diagnostics are also fixed: ld does not apply a non-weak
undefined symbol's placeholder zero relocation after reporting it, and it
does not range-check an unreachable PC-relative call to an undefined weak
symbol. Absolute undefined-weak references still resolve to zero.
`gcc.dg/visibility-22.c` passes both focused verdicts, and the full ld totals
above remain unchanged and free of unexpected results.

### macOS DejaGnu large-file and pipeline transport

Fixed. The compatibility layer now handles GCC drivers which directly open a
read-only Tcl pipeline, so LTO `20081212-1` passes `scan-symbol`. The board's
runtime-gap classifier also restricts source inspection to source-language
files. It previously opened LTO `pr122515`'s 2.88 GB archive as text and
overflowed Tcl 8.5 before the link. The focused upstream replay now completes
with 11 passes and one legitimate `memory full` unsupported result for the
320 MB extracted object. Explicit `-lm` failures and undefined `__gcov_*`
services are classified as the external runtime prerequisites documented
below; unrelated undefined symbols continue to fail.

The complete post-fix LTO driver is clean: 1,651 expected passes, 34
unsupported tests, and zero failures or unresolved cases.

### CTF and optional debug formats

Fixed. Binutils now builds libctf, and the focused ordinary debug replay has
212 passes. C33 gas also supports assembler-generated DWARF location views:
its cons-expression hook now honors the relocation passed by gas instead of
reusing stale global parser state. The hierarchical discriminator tests and
`debug/dwarf2/pr53948.c` pass. `debug/dwarf2/inline5.c` still emits three
target-dependent lexical variable DIE matches where the generic scan expects
one; the generated debug info is otherwise accepted and this has not been
shown to be malformed.

### Plugins and crash diagnostics

Installed plugin headers are now used correctly; the focused plugin run has
869 passes. Remaining failures concentrate in intentional compiler-crash,
SARIF, and diagnostic-path behavior on Darwin. Audit these as host GCC
integration failures without suppressing them.

## P2: explicit compiler feature debt

These are not silently dismissed as "unsupported." They are real possible
extensions, but each needs an ABI/runtime design before implementation:

* `__int128`: define the C33 ABI representation, argument/return convention,
  alignment, TImode moves and arithmetic lowering, and the required libgcc
  helpers. Most 32-bit GCC targets do not expose this type, so its absence is
  not a regression in the existing C33 contract, but it remains an explicit
  feature opportunity.
* atomics: C33 has no native compare-and-swap or thread model. Supporting the
  atomic suites requires enabling/building libatomic and defining how locks,
  interrupt exclusion, and multi-thread visibility work on the target; the
  compiler is currently configured `--disable-libatomic`.
* heap trampolines: ordinary stack trampolines work and
  `gcc.dg/trampoline-1.c` executes successfully. GCC's separate heap
  trampoline method needs an allocator/executable-memory policy and target
  runtime entry points.

Treat these as implementation projects, not as reasons to weaken effective
target probes. They are lower priority than demonstrated wrong code in an
already supported feature.

## External prerequisites - ask before implementing

These failures are useful coverage signals, but their missing implementation
is outside the current GCC/binutils task:

* mini-libc/libm, including C99 math and floating-point `printf`;
* hosted file, environment, time, signal, and process APIs;
* hosted libgcov termination/file output plus persistent `.gcda` transport
  (`libgcov.a` itself is built and installed, including the counter and merge
  machinery, but the freestanding build has no `__gcov_exit` writer);
* C33 sanitizer runtimes and their language-runtime dependencies;
* analyzer-facing hosted-libc declarations and attributes;
* crt startup execution of constructor/destructor arrays; and
* emulator semihosting, persistent files, or a thread/TLS model.

Do not add stubs merely to turn these results green. Classify the prerequisite
with evidence, leave the result visible, and ask before implementing anything
in this section.

## Fixed from the baseline

These are implementation fixes, not unsupported classifications:

* C33 variadic calls now retain their historical stack representation while
  shadowing typed scalar locations and carrying a versioned forwarding
  descriptor in caller-clobbered `%r5`.  C33's `untyped_call` compacts the
  copied stack stream from that descriptor.  The complete focused upstream
  `builtin-apply` and stack-alignment run has 107 passes and zero failures;
  no skip or upstream test change is involved.

* GCC is configured for ELF init/fini arrays. Focused object and link probes
  verify typed priority sections, linker retention and priority ordering, and
  hidden array boundaries. Only crt iteration remains external.

* C33 BFD now permits local-only relocation sections without a global symbol
  hash. `builtins.exp=complex-1.c` passes all 16 verdicts.
* C33's speed branch cost now reflects the core manual: conditional branches
  cost at least two cycles, not GCC's generic one. The focused
  `reassoc-{33,34,35,36}.c` and `update-threading.c` replays now pass all 14
  compile, execution, and optimization assertions.
* Ordinary direct calls now test their symbolic address in pointer mode, not
  the called memory object's `QImode`. The old mismatch forced every direct
  call through a register. Focused assembly probes emit `scall` in short-call
  mode and `xcall` in long-call mode; `weak/typeof-2.c` passes all 8 assertions
  and `tree-ssa/loop-1.c` all 5. The complete weak-symbol driver passes all 93
  assertions.
  The complete post-fix `gcc.c-torture/execute` qualification records 24,260
  passes, 251 legitimate unsupported results, and zero failures or unresolved
  cases across all 1,692 sources and their upstream option variants.
* GCC now emits direct sibling calls after restoring the current frame. Both
  short (`sjp`) and long (`xjp`) forms are implemented; the focused
  `gcc.dg/sibcall-*.c` run has 18 passes and zero failures.
* GCC now preserves a 16-byte outgoing-argument boundary across C33's
  stack-pushed return address. `gcc.dg/pr84877.c` passes, and the DWARF CFA now
  describes the return-address word with `INCOMING_FRAME_SP_OFFSET`.
* The DejaGnu wrapper exposes target headers globally, puts matching binutils
  on `PATH`, finds the prefixed gcov, recognizes ELF weak aliases, and uses
  installed plugin headers from isolated result directories. It also follows
  GCC's standard expensive-test default instead of enabling those cases
  silently.
* GCC uses normal dotted private symbols, recognizes byte/halfword loads from
  `%sp`, and selects standard ELF mergeable constant/string sections.
* C33 BFD keeps section symbols local even for the port's common-section
  encodings, uses modern reserved section indices, and resolves local
  relocations through BFD's merge-aware helper. These fix `pr83100-2`,
  `pr43557-1`, LTO `pr83719`, and LTO `pr50199`; the selected old LTO family
  now has 86 passes and zero failures.
* C33 gas's cons-expression hook uses the caller-supplied relocation and
  handles no-relocation bookkeeping fixups, enabling DWARF `.loc` views.
* Binutils builds libctf; old `-gctf` link warnings are gone.
* The board preserves an explicit test `-w`, reports the actual C99 runtime
  capability, links libc/libgcc archives in resolvable order, and separates
  emulator diagnostics from target program output.
* Precompiled headers pass their complete focused suite: 1,254 passes and no
  failures.

## Remaining compiler-only scan mismatches

The following focused failures remain visible, but their requested scan is
not evidence of incorrect C33 output:

* `ipa-icf-12.c` and `ipa-icf-13.c` find one extra valid identical-function
  pair: mini-libc's inline `abs` and `labs` are identical because C33 `int`
  and `long` are both 32 bits. The expected `gcd`/`nsd` folds also occur.
* `pr87954.c` performs the requested widened multiply once; GCC's dump prints
  it as `w*`, while the test's target-independent expression accepts `*`,
  `*w`, or `WIDEN_MULT_PLUS_EXPR`.
* `ifcvt-4.c` expects a multi-set conditional conversion. C33 has no
  conditional-move instruction and correctly retains a branch.
* `tree-ssa/pr83403-{1,2}.c` perform all ten requested store-motion folds when
  compiled with `--param max-completely-peeled-insns=250`. GCC's default is
  200, and the upstream tests already add 300 for several 32-bit targets but
  do not know C33. Raising the compiler's global peeling budget solely for
  these scans would be a tuning-policy change, not a correctness fix.
* `stack-usage-1.c` reports 272 bytes: the requested 256-byte object plus the
  16-byte `%r0`-through-`%r3` callee-save block. The generic scan accepts only
  256 or 264 unless a target-specific size is listed.
* `debug/dwarf2/inline5.c` emits three lexical-variable DIE matches where the
  generic scan expects one. `readelf --debug-dump=info` confirms the correct
  graph: one abstract block/variable, one inlined concrete instance pointing
  to them, and one out-of-line concrete instance pointing to them. The scan's
  second comment-stopping character class omits C33's `;` assembler comment
  marker, so it traverses the two `DW_AT_abstract_origin` comments and counts
  all three variable DIE annotations. The emitted DWARF is not malformed.

These tests have not been edited or skipped. Revisit a case only if inspection
shows incorrect code, ABI behavior, or malformed debug information rather
than a target-dependent count or spelling.

## Definition of done

A GCC/binutils item is complete when:

1. the implementation fix is understood and covered by focused upstream
   DejaGnu tests;
2. both relevant C33 variants (for example short/long calls or multilibs) are
   checked where applicable;
3. the upstream test is unchanged and no result was hidden by a board skip;
4. the next broad run shows the expected reduction without new regressions.

An external prerequisite is complete only after the user separately
authorizes that non-GCC/binutils work.
