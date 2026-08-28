# gcc.c-torture: what fails

Regenerate with `tests/run-torture.sh execute` and `... compile`. Both run
all seven of upstream's option sets: `-O0`, `-O1`, `-O2`,
`-O3 -fomit-frame-pointer -funroll-loops -fpeel-loops -ftracer -finline-functions`,
`-O3 -g`, `-Os`, `-Og -g`.

## execute - nothing fails, at any level

Verified by a clean full run, 2026-08-27. Identical at all seven sets:

| | pass | unsupported | fail |
|---|---:|---:|---:|
| every option set | **1671** | 21 | **0** |

1692 tests each, 11,844 results. For scale, the baseline two passes ago was
1553 / 1569 / 1534 / 1530 at four levels with 72 distinct tests failing, and
before the runtime work below it was 1611-1615 with 77 unsupported.

### The 21 that cannot run here

Nothing left in this list is about the backend. Five of them are cheap to
recover and worth it - see "What a real stdio would buy" below. The rest is
a libm, a filesystem, a 128-bit integer type, or an x86 register name.

| what they need | tests |
|---|---|
| `FILE *`, `stdout`/`stderr`, `fopen`/`fclose`/`fscanf` | `fprintf-1` `fprintf-2` `fprintf-chk-1` `printf-2` `user-printf` `vfprintf-1` `vfprintf-chk-1` `gofast` |
| `__int128` or decimal float | `pr105613` `pr80692` `pr84748` `pr93213` |
| a C99 math library | `980709-1` `990826-0` `float-floor` `20030125-1` |
| `%f` in printf | `920501-8` `930513-1` |
| `<sys/mman.h>` | `loop-2f` `loop-2g` |
| x86 register names in `asm` | `990413-2` |

`20101011-1` used to be on this list for wanting `<signal.h>`. It does not:
upstream ships `{ dg-additional-options "-DSIGNAL_SUPPRESS" { target { !
signal } } }` for exactly this case, and the harness was applying
`dg-additional-options` while ignoring the target selector on them, so the
flag was never supplied. It passes at all seven sets. **"UNSUPPORTED" in
this harness can mean "the directive was not read", not "the target cannot
do this"** - the classifier is the thing to distrust first.

### What a real stdio would buy: 5 of the remaining 8

The `FILE *` row above is not one problem but two. Five of those tests need
only a `stdout`/`stderr` that reaches `putchar`, which the runtime already
has:

| test | needs | formats used |
|---|---|---|
| `fprintf-1` | `stdout`, `fprintf` | `%c %d %s` |
| `fprintf-chk-1` | `stdout`, `vfprintf` | `%c %d %s` |
| `vfprintf-1` | `stdout`, `vfprintf` | `%c %d %s` |
| `vfprintf-chk-1` | `stdout`, `vfprintf` | `%c %d %s` |
| `gofast` | `stderr`, `fprintf` | `%s` |

Nothing there needs a format mini-libc does not already get right, and
`vuprintf` already returns the character count, which is what `fprintf-1`
asserts - eleven times, for `%c`, `%d`, `%s` and the empty string. That is
the same class of check that caught the `%o` and `%hh` bugs, so these are
worth having rather than merely countable. The `_chk` pair define
`__vfprintf_chk` themselves and call plain `vfprintf`.

The other three - `fprintf-2`, `printf-2`, `user-printf` - want
`fopen`/`freopen`/`fscanf`/`remove`/`tmpnam` against a real filesystem.
That is reachable: `samo-lib/fatfs` (`tff.c`, read-write) and
`samo-lib/drivers` (`sd_spi.c`, `mmc.c`) are firmware code the emulator
already models, and `libtinyfat.a` is prebuilt. The cost is not the
filesystem, it is that **mini-libc has no `scanf` at all**, and that the
harness runs tests in parallel against one image, so each test would need
its own writable card and the runtime would need to bring up SD without
grifo. Three tests for that is a poor trade; the five above are a good one.

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
