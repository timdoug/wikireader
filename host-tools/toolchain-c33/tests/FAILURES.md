# gcc.c-torture: what fails

This file preserves the bootstrap shell runner's historical results and the
bugs found while qualifying the C33 DejaGnu board. The shell runner has been
retired. Current results come from `dejagnu/run-dejagnu.sh` and DejaGnu's
standard `gcc.sum` and `gcc.log` files.

## execute - nothing fails, at any level

Verified by a clean full run, 2026-08-28. Identical at all seven sets:

| | pass | unsupported | fail |
|---|---:|---:|---:|
| every option set | **1676** | 16 | **0** |

1692 tests each, 11,844 results. For scale, the baseline two passes ago was
1553 / 1569 / 1534 / 1530 at four levels with 72 distinct tests failing, and
before the runtime work below it was 1611-1615 with 77 unsupported.

### The 16 that cannot run here

Nothing left in this list is about the backend, and none of it is cheap:
it is a libm, a filesystem, a 128-bit integer type, or an x86 register
name.

| what they need | tests |
|---|---|
| `fopen`/`fclose`/`fscanf` on a real filesystem | `fprintf-2` `printf-2` `user-printf` |
| `__int128` or decimal float | `pr105613` `pr80692` `pr84748` `pr93213` |
| a C99 math library | `980709-1` `990826-0` `float-floor` `20030125-1` |
| `%f` in printf - deliberately | `920501-8` `930513-1` |
| `<sys/mman.h>` | `loop-2f` `loop-2g` |
| x86 register names in `asm` | `990413-2` |

`990413-2` is the only test in the whole suite that declares itself for
other architectures with the negated form,
`{ dg-skip-if "" { ! { i?86-*-* x86_64-*-* } } }`. Upstream's own runners do
nothing special with it - DejaGnu reads the directive and never runs it, on
aarch64 or anywhere else. We reached the same answer by a different route:
the x87 asm constraints failed to compile with "invalid register name" and
`unsupported_p` greps for that string. `wrong_target_p` now reads the
directive instead. The outcome is unchanged; the reason it holds is no
longer a diagnostic upstream is free to reword.

### `%f` is a decision, not a gap

mini-libc has no `%f` and is keeping it that way. Adding one pulls
`__adddf3`/`__subdf3` (858 bytes), `__muldf3` (594), `__divdf3` (348), the
`df` comparison set and `__fixdfsi` into **every program that links
printf** - about 3 KB. Measured against the current build: neither
`wiki.app` nor `grifo.elf` contains a single double soft-float symbol, and
no format string anywhere in `samo-lib`, `wiki` or `host-tools` uses `%f`,
`%e` or `%g`. Two tests is not worth 3 KB in the shipped library of a
device with fixed flash.

Neither test is really about float formatting in any case. `920501-8` is a
varargs test - a `double` in argument slot 2 followed by thirteen
`va_arg(ap, int)` - and `930513-1` calls `sprintf` through a K&R-declared
function pointer. The argument passing they both lean on is covered
directly, and against the 3.3.2 oracle, by `tests/abi`.

If they are ever wanted, the way that does not touch the firmware is a
`sprintf` in `tests/runtime/` shadowing mini-libc's, since a runtime object
beats an archive member at link time. That buys the two results at the cost
of testing a formatter the device does not have, which is weaker coverage
than it looks.

### Reading directives beats grepping diagnostics

`20101011-1` used to be on this list for wanting `<signal.h>`. It does not:
upstream ships `{ dg-additional-options "-DSIGNAL_SUPPRESS" { target { !
signal } } }` for exactly this case, and the harness was applying
`dg-additional-options` while ignoring the target selector on them, so the
flag was never supplied. It passes at all seven sets. **"UNSUPPORTED" in
this harness can mean "the directive was not read", not "the target cannot
do this"** - the classifier is the thing to distrust first.

### Streams: five recovered, and they earn their place

That row used to hold eight tests and be labelled "needs `FILE *`". It was
two problems wearing one label. Five of them wanted nothing but a `stdout`
or `stderr` that reaches `putchar` - which the runtime already had - and
used only `%c`, `%d` and `%s`, which mini-libc already formats correctly:
`fprintf-1`, `fprintf-chk-1`, `vfprintf-1`, `vfprintf-chk-1`, `gofast`.
They now pass at all seven sets.

`runtime.c` gains `stdout`/`stderr`/`stdin` and `fprintf`/`vfprintf` over
`vuprintf(putchar, ...)`, with the `FILE *` ignored; `include/stdio.h`
layers that onto mini-libc's via `#include_next`. Two things are worth
knowing before touching it:

* **`fputc`, `fputs`, `fwrite`, `putc` and `fflush` have to exist even
  though no test names them.** GCC rewrites the printf family when the
  format is simple - `fprintf(f, "s")` becomes `fputs`, `fprintf(f, "%c",
  c)` becomes `fputc`. Omitting them turns a test that would have passed
  into a link failure reported as UNSUPPORTED, which is the exact failure
  mode this file exists to remove.
* **The point of these tests is the return value, not the output.**
  `fprintf-1` checks the count eleven times, over `%c`, `%d`, `%s` and the
  empty string. GCC folds those counts at compile time under
  `-fprintf-return-value`, so a disagreement between the folded constant
  and what the library writes is a hard failure - the same check that
  caught the `%o` and `%hh` bugs. Confirmed by reading the serial capture:
  the emitted bytes match what the test intends, so it is running rather
  than being folded away.

The `_chk` pair define `__vfprintf_chk` themselves and call plain
`vfprintf`, so they need nothing extra.

The other three - `fprintf-2`, `printf-2`, `user-printf` - want
`fopen`/`freopen`/`fscanf`/`remove`/`tmpnam` against a real filesystem, and
stay on the list. That is *reachable*: `samo-lib/fatfs` (`tff.c`,
read-write) and `samo-lib/drivers` (`sd_spi.c`, `mmc.c`) are firmware code
the emulator already models, and `libtinyfat.a` is prebuilt. The cost is
not the filesystem. It is that **mini-libc has no `scanf` at all**, and
that the harness runs tests in parallel against one card image, so each
test would need its own writable card and the runtime would need to bring
up SD without grifo. Three tests for that is a poor trade.

Most of these are detected from the compiler's own diagnostics or from
`{ dg-do ... { target ... } }`. Three are not: `920501-8`, `930513-1` and
`20030125-1` build and run and then abort, exactly as a miscompilation
would, so nothing in the output distinguishes them. They are named
individually in `skip_reason()` with what each wants. That list is the
weakest thing here: a real miscompilation in one of those three would look
identical to the libc gap being claimed.

`pr78622` and `pr79327` used to be on it and are not any more, because the
libc gap they were blocked on turned out to be two real bugs in mini-libc's
printf - see below.

### What made the difference: the runtime, not the compiler

53 of the previous 77 unsupported were unsupported only because they
referenced a symbol nothing defined. Four gaps accounted for nearly all of
it, and `tests/runtime/` now fills them:

* **`exit()`** - mini-libc declares it `__asm__("__stop_progExec__")`, so
  every test including `<stdlib.h>` linked against that name and not
  `exit`. One label in `crt0.s`; 38 tests referenced it.
* **`putchar`** - mini-libc's `printf` calls `vuprintf(putchar, ...)` and
  expects the user to supply `putchar`. It writes to `REG_EFSIF0_TXD`, so a
  failing test's own output now lands in the emulator's serial capture.
* **`malloc`/`free`/`calloc`** - thin wrappers over **grifo's allocator**,
  compiled straight from `samo-lib/grifo/src/memory.c`. Reusing the
  firmware's rather than writing one means these tests also put real
  firmware code through the new compiler. No `realloc`: grifo has no
  equivalent and faking one means duplicating `memory.c`'s header layout
  here. Nothing needs it, and a test that did would fail to link - which
  reports UNSUPPORTED rather than something silently wrong.
* **`setjmp`/`longjmp`** - the one piece with no firmware equivalent, so it
  is written here. The C33 has no link register: `call` pushes the return
  address and `ret` pops it, so the buffer is `%r0`-`%r3`, `%sp`, and the
  word at `[%sp+0]` on entry.

### Two real printf bugs, found by tests that could not run

`pr78622` and `pr79327` were on the skip list for wanting `%hhd` and
`%#hho`/`%#hhx`. Both turned out to be genuine bugs in mini-libc, and the
compiler was right all along -- what the tests detect is GCC's computed
`sprintf` return value disagreeing with what the library actually writes:

```
before                          after
[%o of 8]       -> '0'   n=2    [%o of 8]       -> '10'   n=2
[%#o of 8]      -> '0'   n=3    [%#o of 8]      -> '010'  n=3
[%hhd of 300]   -> '300' n=2    [%hhd of 300]   -> '44'   n=2
[%hhx of 0x1ff] -> '1ff' n=2    [%hhx of 0x1ff] -> 'ff'   n=2
```

Two separate faults. `%o` and the BSD `%O`/`%U` spellings never fetched
their argument at all -- the prefetch covered only `u`, `x` and `X`, so
`printf("%o", 8)` formatted whatever `_ulong` last held, and consumed no
vararg, which desynchronised everything after it in the format. And a
second `h` just set `SHORTINT` again, so `%hhd` behaved as `%hd`.

Note the `n=` column: GCC's folded return value was correct in every case
before the fix. `-fprintf-return-value` was right and the library was
wrong, which is exactly what these two tests exist to catch.

Fixed in `mini-libc/src/stdlib/vuprintf.c`. Both tests now pass at all
seven option sets, the firmware builds, and `wiki.app` renders a screen
identical to the build before the change.

## How much to trust these numbers

The retired harness was a shell script rather than DejaGnu, and every
classification in it was written by hand. Its audit found three real
problems and four remaining soft spots. These notes explain why its totals
must not be treated as current results.

### What the pass criterion actually rests on

A test passes if the emulator reaches the exit breakpoint with `%r4 == 0`.
That is honest for a specific reason: **`abort` is defined in `crt0.s`**, not
taken from mini-libc, and it exits with `%r4 == 0xdead`. A test object file
beats an archive member at link time, so a torture test that calls `abort`
 - which is how every one of them reports a wrong answer - can never be
confused with success. Faults are matched separately, a non-zero status is
reported as `EXITnn`, never reaching the breakpoint is `TIMEOUT`, and a run
that produced no register dump at all is `NORUN` rather than being folded
into `TIMEOUT`. No test source is ever edited.

### Three things that were wrong

* **`expects_error_p` did not check the error.** It was `grep -q dg-error`
  on the *source*: any compile failure in a file that mentioned `dg-error`
  anywhere was reported PASS, whatever the failure was. Five compile tests
  carry `dg-error`; all five did produce the expected diagnostic, so nothing
  was actually mis-scored - but nothing was checking, and a backend failure
  in one of those files would have read as success. It now matches the log
  against the messages the test asks for.

  Fixing it introduced two worse bugs, both caught by running the full suite
  rather than a subset, and both worth knowing about:

  1. The matching loop was on the right-hand side of a pipe, so it ran in a
     subshell, and a `while` loop that simply *ends* exits 0. The function
     returned true for every file and **every failed compile became a PASS**
 - 70 results, 10 tests per option set, silently moved from UNSUPPORTED
     to PASS.
  2. With that fixed, matching the pattern against the whole log still
     passed a test that failed with entirely the wrong diagnostic. GCC's
     caret output **echoes the offending source line**, and that line is
     where `dg-error` lives - so the pattern was matching the directive
     quoting itself. It now looks only at `file:line:col: error:` lines.

  There is a negative test for this: a file that fails to compile with a
  message it does not claim to expect must report FAIL, and does.

* **`undefined reference` meant UNSUPPORTED, unconditionally.** This is the
  one that mattered most. A backend that emits a call to a libgcc helper
  that does not exist, or gets a libcall name wrong, produces exactly that
  message - and it was being filed as "a gap in our libc, not our problem".
  That is the class of bug this port keeps producing. A link failure now
  counts as a gap only if **every** symbol it names is on an explicit list
  of things we knowingly do not provide (`fopen`, `floor`, `mmap`, ...);
  anything else is a FAIL. Verified by planting a call to a nonexistent
  helper: it reports FAIL, where before it would have been UNSUPPORTED.

* **Target selectors on `dg-` directives were ignored.** That cost
  `20101011-1` for a whole session - see above. `990413-2` now classifies
  from its own `dg-skip-if` rather than from a diagnostic string.

### Four things that remained soft in the shell harness

1. **`-w -fpermissive`.** All warnings off, and some errors downgraded. It
   is what lets pre-C23 sources through at all, but it does mean the suite
   is not checking that the compiler rejects what it should. Every "compile"
   result is really "produced assembly without an ICE" - not "produced
   *correct* assembly".
2. **Three tests are skipped by name** - `920501-8`, `930513-1`,
   `20030125-1`. They build, run and abort, which is indistinguishable from
   a miscompilation. The reasons are recorded and are believed, but they are
   believed rather than demonstrated. This is the weakest claim here.
3. **`comp-goto-1` gets `-std=gnu89` that upstream does not give it.**
   Upstream puts it in `dg-options` on most of the pre-ANSI tests and missed
   this one; that is a judgement call we made, and it is us changing how a
   test compiles in order to make it pass.
4. **`wrong_target_p` guesses.** It has no model of dg's effective-target
   vocabulary - it treats "contains a dash" as "is a target triplet" and
   knows `lp64` is false. Effective targets we satisfy are deliberately left
   alone rather than guessed at, but it is a heuristic.

The real fix for most of 1-4 is implemented in `dejagnu/`. GCC's own
driver evaluates the directives, selectively re-enables warnings for tests
that assert diagnostics, and uses an explicit runtime-capability boundary.
It found additional shell-harness classification errors: three compile tests
were skipped only because their basenames collide with execute-only skips,
`! llp64` was read backwards, and requirements for weak aliases, retained
sections, profiling, and option-specific skips were ignored. The DejaGnu
wrapper follows GCC's default and runs expensive tests only when requested.
The bootstrap harness was removed after this cross-check; upstream DejaGnu
results are authoritative.

## compile - 1973 of 2003 per set, no failures, no ICEs

| | pass | ICE | fail | unsupported |
|---|---:|---:|---:|---:|
| every option set | **1973** | 0 | **0** | 30 |

14,021 results. The ICE below is gone as of the argument-passing fix.

The 30 unsupported are another architecture's `dg-options` (`-mavx`,
`-march=skylake`, `-mcpu=603e`, `-pthread`), another architecture's
`dg-do` target selector (x86, MIPS, `lp64`), `__int128`, or
`__declspec(dllimport)`.

The last two of those - `pr99822` and `dll` - were reported `FAIL` until
their diagnostics were added to `unsupported_p`, which matched
`unknown type name` but not `expected expression before '__int128'` or
`before '__declspec'`. Worth knowing when changing that function:
**it is only consulted after a compile has already failed**, so it cannot
reclassify anything that passes. The only tests it can reach are the ones
already failing to compile, which makes re-running just those a complete
check rather than a sample.

### The one ICE - which was ours, not upstream's

**Resolved.** `pr110266` now passes at all seven option sets. The analysis
below is kept because the *mechanism* it describes was correct and the
*conclusion* was wrong, which is the more useful thing to remember.

It used to ICE at `-O0` and `-Og -g` - the levels that do not fold the
guarding `if` away before expand:

```c
double PsyBufferUpdate (int n)
{
  if (n == 4)
    {
      _Complex double t = __builtin_cexpi (n);
      return __real t * __imag t;
    }
  return 0;
}
```

```
internal compiler error: in expand_expr_addr_expr_1, at expr.cc:9343
```

This was written up as **upstream's, not ours**. Every step of it is true
except the one that mattered:

* The failing code is entirely generic - `expand_builtin_cexpi` in
  `gcc/builtins.cc` calling into `gcc/expr.cc`. Nothing in `config/c33` is
  on the path.
* Reaching it needs two target properties, neither of which we chose
  wrongly. `elfos.h` - not us - sets `TARGET_LIBC_HAS_FUNCTION` to
  `no_c99_libc_has_function`, so `expand_builtin_cexpi` falls past its
  `sincos` optab and `sincos` libcall paths to the `cexp` one, where it
  builds a `COMPLEX_EXPR` *rvalue* and passes it by value. And a
  `_Complex double` is 16 bytes, so this ABI passes it in memory, so expand
  must take that rvalue's address - and `get_inner_reference` makes no
  progress on a `COMPLEX_EXPR`: `gcc_assert (inner != exp)`.
* Both halves are necessary, confirmed by experiment. `__builtin_cexpif`
  gives a `_Complex float`, 8 bytes, passed in registers - **compiles
  fine**. And an ordinary 16-byte `struct { double a, b; }` returned by
  value **compiles fine**, so memory-passed aggregates are not broken in
  general.

The conclusion drawn from that was: "So the ABI is right (>8 bytes to memory,
inherited from gcc 3.3.2) and the configuration is right; upstream simply has
a path that assumes the `COMPLEX_EXPR` never needs an address."

**The ABI was not right, and it was not inherited from gcc 3.3.2.** 3.3.2
passes a `_Complex double` by value in `%r6`-`%r9`; the >8-bytes-to-memory
rule was ours, a V850 inheritance via `c33_pass_by_reference`, and it is
exactly what forced expand to take the address. With the argument-passing
hooks corrected the value goes in registers and the path is never reached.

Worth extracting, because the write-up reads as thorough and was wrong
anyway:

* The two controls (`__builtin_cexpif` compiles, a 16-byte struct return
  compiles) were real and correctly interpreted. They isolated
  *memory-passing of this particular rvalue* as the trigger - and then that
  finding was used to exonerate the ABI rather than to interrogate it.
* The one claim carrying the whole argument, "inherited from gcc 3.3.2", was
  the only one never checked against the oracle. It would have taken one
  probe.
* An ICE reached through a target-dependent path is not upstream's until the
  target's behaviour on that path has been compared with the oracle.
