# gcc.c-torture: what fails

Regenerate with `tests/run-torture.sh execute` and `... compile`. Both run
all seven of upstream's option sets: `-O0`, `-O1`, `-O2`,
`-O3 -fomit-frame-pointer -funroll-loops -fpeel-loops -ftracer -finline-functions`,
`-O3 -g`, `-Os`, `-Og -g`.

## execute - nothing fails, at any level

Verified by a clean full run, 2026-08-27. Identical at all seven sets:

| | pass | unsupported | fail |
|---|---:|---:|---:|
| every option set | **1668** | 24 | **0** |

1692 tests each, 11,844 results. For scale, the baseline two passes ago was
1553 / 1569 / 1534 / 1530 at four levels with 72 distinct tests failing, and
before the runtime work below it was 1611-1615 with 77 unsupported.

### The 24 that cannot run here

Nothing left in this list is about the backend, and none of it is cheap to
recover - it is a libm, a filesystem, or a 128-bit integer type.

| what they need | tests |
|---|---|
| `FILE *`, `stdout`/`stderr`, `fopen`/`fclose`/`fscanf` | `fprintf-1` `fprintf-2` `fprintf-chk-1` `printf-2` `user-printf` `vfprintf-1` `vfprintf-chk-1` `gofast` |
| `__int128` or decimal float | `pr105613` `pr80692` `pr84748` `pr93213` |
| a C99 math library | `980709-1` `990826-0` `float-floor` `20030125-1` |
| `%f`, `%hhd` or `%#hhx` in printf | `920501-8` `930513-1` `pr78622` `pr79327` |
| `<sys/mman.h>` | `loop-2f` `loop-2g` |
| `<signal.h>` | `20101011-1` |
| x86 register names in `asm` | `990413-2` |

The harness detects the first six groups from the compiler's own
diagnostics or from `{ dg-do ... { target ... } }`. The four printf ones
build and run and then abort, exactly as a miscompilation would, so nothing
in the output distinguishes them - they are named individually in
`skip_reason()` in the harness, with what each wants.

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

## compile - 1972/1973 of 2003 per set, one ICE

| | pass | ICE | fail | unsupported |
|---|---:|---:|---:|---:|
| `-O0` and `-Og -g` | 1972 | 1 | 2 | 28 |
| the other five | 1973 | 0 | 2 | 28 |

The 28 unsupported are another architecture's `dg-options` (`-mavx`,
`-march=skylake`, `-mcpu=603e`, `-pthread`), another architecture's
`dg-do` target selector (x86, MIPS, `lp64`), or `__int128`.

Two `FAIL`s remain, both classified and neither codegen:

* `pr99822` - wants `__int128`. The message is `expected expression before
  '__int128'`, which the unsupported-detector's pattern does not match; it
  looks for `unknown type name`. Cosmetic.
* `dll` - `__declspec(dllimport)` on a parameter. A Windows test.

### The one ICE

`pr110266`, at `-O0` and `-Og -g` - the levels that do not fold the
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

**It is upstream's, not ours**, and the evidence is direct:

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

So the ABI is right (>8 bytes to memory, inherited from gcc 3.3.2) and the
configuration is right; upstream simply has a path that assumes the
`COMPLEX_EXPR` never needs an address. It cannot be fixed inside
`config/c33` without lying about the target - claiming a `sincos` pattern
or a C99 libm we do not have. Either carry a local `builtins.cc` patch
forcing that rvalue into a temporary, or report it and leave it. No program
that can run on this device is affected: there is no complex math library
to call.
