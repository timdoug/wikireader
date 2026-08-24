# gcc 3.3.2 (c33) discards a narrowing signed cast - FIXED

Found by the differential harness at seed 49. This is a **compiler** bug, not
an emulator bug: the emulator faithfully executes what the compiler emitted.

**Fixed** by `host-tools/toolchain-patches/0008-gcc-Fix-fold-discarding-a-narrowing-cast-in-extract_.patch`.
This file is kept as the reduction and the regression case.

## Reduction

```c
u32 s = 0x85b48108u;
(u32)(signed char)(s * 0xdeadbeefu) * 3u
```

C says: `s * 0xdeadbeef` is `0xedb06678`; `(signed char)` of that is `0x78`
(120); times 3 is **360**.

gcc 3.3.2 for c33 at `-O1` and above says **0xc9113368**, which is
`s * (0xdeadbeef * 3)` - it has fused the two multiplies and dropped the
cast. The generated code shows it plainly:

```
	ld.w	%r6,[%r15]              ; load s
	xld.w	%r4,-1677116211	;0x9c093ccd     <- 0xdeadbeef * 3
	mlt.w	%r6,%r4
	ld.w	%r6,%alr
```

There is no sign-extension instruction anywhere in that sequence.

## Scope

* Needs a narrowing **signed** cast: `signed char` and `short` both
  miscompile, `unsigned char` is correct.
* Needs the cast operand to be a **multiply**: `(signed char)(s + 1) * C` is
  correct.
* Needs a **multiply** after the cast: `+` and `^` after the cast are correct.
* Correct at `-O0`, wrong at `-O1`, `-O2` and `-Os`.
* It is not merely constant folding. The `volatile` variant, which the
  compiler cannot fold, is miscompiled the same way - the emitted
  instructions implement the wrong expression.

## Root cause

`extract_muldiv_1` in `gcc/fold-const.c` guards against folding a multiply
through a truncating conversion with

```c
GET_MODE_SIZE (TYPE_MODE (ctype)) < GET_MODE_SIZE (TYPE_MODE (TREE_TYPE (op0)))
```

but `ctype` is not this node's type. At the top of the function,

```c
ctype = (wide_type != 0 && wider than type) ? wide_type : type;
```

and the `CONVERT_EXPR` case passes `ctype` down as the next `wide_type`. On
the inner conversion node `type` is `signed char` while `ctype` has become
`unsigned int`; both `TYPE_MODE` sizes in the test are 4, the comparison is
false, and the 8-bit narrowing is invisible. The fix tests `type` - the type
this conversion converts *to*, which is what "is this a truncation" is
actually asking. Widening is unaffected: for `(long long)(int)` the test is
`8 < 4`, still false.

## Does it affect the WikiReader firmware?

**No.** Two independent checks.

The compiler was temporarily instrumented to report every site where the new
guard fires, and the entire firmware was rebuilt: **268 translation units
across mini-libc, fatfs, drivers, grifo, wiki, forth and mbr, with zero
sites reached.**

And rebuilding `grifo.elf` with the fixed compiler gives `.text`, `.rodata`
and `.data` byte-identical to the binary built with the unpatched compiler.
That comparison also happens to confirm the native macOS toolchain produces
bit-for-bit the same output as the Ubuntu 12.10 i386 VM.

`libgcc.a` is clean too, and it needed checking separately because it ships
with the toolchain rather than being built from the firmware sources. Two
`cc1` binaries were built from the same tree with the patch as the only
difference, and libgcc built with each: **all 57 objects byte-identical**.
So the software divide, the 64-bit helpers and the soft-float code are
unaffected.

(Reproducing that needs `GCC_FOR_TARGET` overridden - `gcc/config/c33/t-c33`
hardcodes `d:/Epson/gnu33/xgcc`, a path from EPSON's original Windows build,
which makes `mklibgcc` emit an empty `libgcc.mk` and silently skip libgcc
entirely:

```
make libgcc.mk libgcc.a GCC_FOR_TARGET="./xgcc -B./"
```

The result reproduces the shipped archive: all 57 `.text` sections match the
installed `libgcc.a`, with whole-object differences confined to stabs debug
paths.)

## Running it

```
./compiler-bugs/run.sh
```

Prints the folded value, the run-time value and what C requires. Both target
values come from the emulator, so the demonstration does not depend on the
emulator being correct - it only depends on it being consistent, and the two
paths through it are independent.
