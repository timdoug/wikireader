# gcc.c-torture: what fails

Regenerate with `tests/run-torture.sh execute` and `... compile`.

## execute - nothing fails

Verified by a clean full run, all four levels, 2026-08-27.

| | pass | unsupported | fail |
|---|---:|---:|---:|
| `-O0` | 1611 | 81 | **0** |
| `-O1` | 1615 | 77 | **0** |
| `-O2` | 1615 | 77 | **0** |
| `-Os` | 1615 | 77 | **0** |

1692 tests each. The four extra unsupported at `-O0` are `20000914-1`,
`complex-6`, `pr103405` and `pr15262-1`, which call `malloc`; at `-O1`
and above it gets optimised away and they link.

The previous baseline, for scale: 1553 / 1569 / 1534 / 1530, with 72
distinct tests failing.

### What "unsupported" covers

77 of 1692, and none of them are about the backend. The harness reports a
test unsupported when it cannot be built or run here at all, which it
detects three ways:

* **From the compiler's own diagnostics** - a header mini-libc does not
  have (`math.h`, `setjmp.h`, `signal.h`, `sys/mman.h`), `__int128`,
  decimal float, an x86 or MIPS register name in an `asm`, an option this
  target does not take, an undefined reference, or `stdout`/`stderr`,
  which this runtime has no concept of.
* **From `{ dg-do ... { target ... } }`** when the selector is a triplet
  for another architecture, or `lp64`.
* **From a four-entry list in the harness**, for tests that build and run
  but need a libc feature mini-libc lacks, so they abort exactly as a
  miscompilation would and nothing in the output distinguishes them:
  `920501-8` and `930513-1` want `%f` in `sprintf`, `pr79327` wants
  `%#hho`/`%#hhx`, `20030125-1` wants a C99 math library to fold `sin`
  and `floor` against.

Upstream skips most of these itself, with `dg-skip-if { freestanding }`
or `dg-require-effective-target c99_runtime`. This harness does not model
effective targets: a blanket skip on those directives would also throw
away a dozen tests that do pass here, so the narrower rules above are
used instead and the residue is listed by name.

## compile - 1975 of 2003 per level, one ICE

Numbers below are from the last **verified** full run, which used the
harness as it stood before the compile-mode refinements described in
"Next steps". They will improve when that is re-run.

| | pass | ICE | fail | unsupported |
|---|---:|---:|---:|---:|
| `-O0` | 1975 | 1 | 22 | 5 |
| `-O1` | 1975 | 0 | 23 | 5 |
| `-O2` | 1975 | 0 | 23 | 5 |
| `-Os` | 1975 | 0 | 23 | 5 |

The 23 `FAIL`s were read individually and every one is the harness or the
target, not codegen:

| what | tests |
|---|---|
| `dg-options` naming another architecture's flags (`-mavx`, `-march=skylake`, `-mcpu=603e`, `-pthread`) | `pr110386-2` `pr88423` `pr88347` `pr86636` `pr86637-2` `pr89235` |
| `dg-do compile { target i?86 / mips / lp64 }` | `asmgoto-3` `pr30311` `mipscop-1..4` `pr65680` |
| needs `__int128` | `bitfield-1` `bitfield-endian-1` `bitfield-endian-2` `pr99822` |
| the test *expects* a diagnostic (`dg-error`) | `pr83547` `pr48767` `20030305-1` `pr28865` |
| K&R storage class on a parameter | `dll` |

### The one ICE

`pr110266`, at `-O0` only:

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

It is not in the c33 backend. `expand_builtin_cexpi` (`gcc/builtins.cc`)
has three paths: a `sincos` optab, a `sincos` libcall, and a `cexp`
libcall. We have no `sincos` pattern, and `elfos.h` sets
`TARGET_LIBC_HAS_FUNCTION` to `no_c99_libc_has_function`, so it takes the
third - where it builds a `COMPLEX_EXPR` rvalue and calls `cexp` with it.
A `_Complex double` is 16 bytes, so this ABI passes it in memory, so
expand has to take the address of that rvalue, and `get_inner_reference`
makes no progress on a `COMPLEX_EXPR`: `gcc_assert (inner != exp)`.

So it needs both halves - a target with no C99 complex math *and* one
that passes `_Complex double` in memory. Fixing it means patching generic
GCC, which nothing in this port has done so far. It was invisible until
this session because the harness passed `-fno-builtin`.

## Next steps

1. **Re-run the compile suite.** `run-torture.sh` has uncommitted-at-time-
   of-writing changes for compile mode - honouring `dg-do ... { target }`,
   treating a `dg-error` test as passing when the compiler diagnoses
   rather than crashes, and reporting unrecognised options and
   `__int128` as unsupported. Each was checked by hand against the tests
   above; none has been run over the suite.
2. **Decide what to do about `pr110266`.** Either patch `builtins.cc` to
   force the `COMPLEX_EXPR` into a temporary and carry it as a local
   change, or record it as a known upstream limitation and move on. It
   affects no program that could run on this device: there is no complex
   math library to call.
3. **Widen the net.** `-O3`, and `gcc.dg` - the torture suite is now
   clean enough that it has stopped being the binding constraint.
