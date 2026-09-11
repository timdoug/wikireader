# The Epson C33 PE target

This checkout adds a C33 PE backend to TinyCC: `c33-gen.c` generates code,
`c33-link.c` relocates it, and `c33-run.c` executes it in place. The target
is used by a NuttX application that compiles C **on an emulated WikiReader**,
so the compiler runs on the C33 as well as generating code for it.

The NuttX side of that port -- the NSH command, the target headers, the
runtime the generated code links against and the application build -- is not
here. It lives in `apps/interpreters/tinycc`, which finds this checkout
through `CONFIG_INTERPRETERS_TCC_SRCDIR`; see its README for building,
running and self-hosting. Physical WikiReader hardware has not been tested.

## Coverage and limits

Tested: preprocessing and headers; signed/unsigned 8/16/32-bit types; 64-bit
integer arithmetic; pointers, arrays, globals and BSS; function pointers;
recursion; short-circuit expressions; loops and switches; bitfields; structs
passed/returned by value; variadic calls/functions; variable-length arrays;
frames past the local-addressing bias; allocation and string routines;
`printf`; ELF object save/reload; compiler errors and unresolved symbols;
repeated invocations; Ctrl-C while running.

Single/double precision and 64-bit `long double` use GCC's software floating
point routines. Tests cover arithmetic, conversions, NaNs, infinities,
negative zero, and interoperability with GCC's floating-point calling
convention.

Inline assembly, packed/over-aligned types, TLS, constructors/destructors,
shared libraries and a full installed C sysroot are not supported. The
runtime exports the functions declared in the application's `include`
directory plus the GCC integer arithmetic helpers. Unsupported generated
operations report errors. This is not yet a general-purpose replacement for
the GCC toolchain. There is no standalone NuttX executable loader: use
`tcc -run` for both sources and objects.

## Backend notes

`c33-gen.c` emits instruction halfwords directly. Its allocator uses r4-r9,
r0 and r1 are saved scratch registers, r3 is a saved frame pointer and r2 a
saved copy of it, biased 8 KB lower. The
floating-point allocator uses r10:r11 and r12:r13 as disjoint register pairs;
function calls and returns transfer floating-point results through r4:r5. GCC's
data pointer in r15 is preserved. Outgoing call stacks are 16-byte aligned;
natural scalar alignment is four bytes, including `long long`.

The biased copy exists because the extension-prefixed `[%rN+disp]`
displacement is unsigned, and locals live below the frame pointer. Addressing
them from r2 turns a seven-instruction address computation, which also had to
save and restore the flags for 64-bit carry/borrow chains, into one prefix and
the access. A frame deeper than the bias still computes the address in r2 and
rebuilds it afterwards, still under the saved flags. `regression.c` covers
that path with a 10 KB frame whose 64-bit accumulator also lives out there.

Two more encodings follow from the same observation that a prefix is an
instruction. A branch is a prefix and the instruction, with the patch chain
held in the field the displacement will occupy, so a chain offset and a branch
distance are both 21 bits; a backward jump that already knows a nearby target
drops the prefix entirely. And a call whose arguments are all word-sized
scalars already in memory loads them straight into r6-r9, because no such load
can overwrite a value a later one still has to read; the general path below it
still parks arguments in an outgoing scratch area first.

Locals stay in memory and every use reloads them, which is the largest cost
left, but caching what the registers hold does not pay for itself. Tried and
measured: track the frame slot in each register, drop the entry on any write
to that register, any store that could alias, every call, and every branch
and label, then turn a reload of a slot still in a register into a move. Only
**9%** of local loads hit. A static scan suggests 32%, but it misses that the
core is two-address, so the arithmetic consuming a loaded value overwrites
the very register holding it. The generated compiler came out 1.1% smaller on
identical source, and `tcc -selfhost` plus a rebuild came out **3.4% slower**:
the tracking itself is code in the compiler, and in the second generation
that code is unoptimised output of this backend, so it costs more than it
saves. Reclaiming those reloads needs real register allocation, not a
peephole - and on this benchmark any allocator has to be cheap enough to pay
for its own compilation twice.

`c33-link.c` implements ELF32 RELA data relocations and C33 H/M/L address
relocations. Objects use machine number 107 and the PE flag `0x50000000`, and
can be linked with the existing GNU C33 linker. GCC objects that use other
relocation types are not accepted by the in-memory linker.

The calling convention includes the unusual r9:r10 `long long` argument pair.
A double at the same position goes on the stack and leaves r9 available.
Scalar and aggregate arguments use independent register/stack streams. GCC
callers duplicate the last named variadic parameter in registers
and on the stack; anonymous arguments follow on the stack. TinyCC does not
implement GCC's `__builtin_apply` forwarding extension.

`c33-run.c` links into allocated SDRAM without mmap/mprotect. The current
NuttX board runs that memory uncached. Enabling instruction or data caches
later will require an appropriate synchronization step before executing
generated code.

## Bootstrap speed

Rebuilding the compiler with the compiler it just produced takes about 52
seconds of guest time at the WikiReader's 48 MHz, against 24 for the same
source under the GCC-built compiler, because this backend performs none of
GCC's optimizations. Time those stages through the WikiReader's FLASH/card
boot, not a direct ELF boot: the latter leaves the SDRAM controller
unprogrammed and its wait states unmodeled, which flatters every measurement
by roughly a factor of four. The board README has the measured breakdown.

## Validation

`tests/c33/run-abi.py` builds a host `c33-tcc` with the standard TinyCC
configure and Makefile, then executes all four GCC/TinyCC caller/callee
combinations under wremu. It covers narrow and wide structs, hidden result
pointers, register overflow, 64-bit arguments, variadic calls and callbacks.
Logs and images go under `build/wikireader/tinycc-*`. The host compiler has
additionally compiled the integer and ABI fixtures under AddressSanitizer and
UndefinedBehaviorSanitizer.

```sh
python3 tinycc/tests/c33/run-abi.py
```

The on-device tests -- native compilation, object save and reload, error
recovery, and three self-hosted compiler generations compared byte for byte
-- are driven from the board and described in
`apps/interpreters/tinycc/README.md`. No physical device, card, or files in
the WikiReader checkout were modified for this port.
