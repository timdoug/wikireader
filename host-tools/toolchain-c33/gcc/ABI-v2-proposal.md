# C33 ABI v2 proposal

Status: deferred experiment after ABI-compatible improvements. The current toolchain
still implements [the existing ABI](ABI.md). This document changes no shipped
or built interface by itself.

Bank the existing ABI's compiler and runtime improvements first: prefer low
preserved registers while retaining the established contiguous saves, and fix
Linux's synthetic signal-entry alignment and alternate-stack bounds check.
Correctness and implementation simplicity take priority over speculative
speedups. These need no new register contract,
argument transport, ELF ABI version or factory boot boundary. Record their
results in [the toolchain guide](../README.md) and use the corrected compiler
and runtime as the baseline for any later v2 experiment. The design below
remains an option, rather than the next required implementation project.

Build a new C33 PE ABI for the software we control: Grifo and its launcher,
ZIM, Game Boy, Doom, NuttX and its native compiler, and native Linux with
uClibc, shared libraries, threads and C++. Optimize ordinary calls, register
residence and stack traffic. Keep the factory flash loading contract at a
small assembly boundary; rebuild the software on the SD card together rather
than retaining the historical calling convention throughout it.

The recommended starting candidate has six preserved registers, six argument
words, return values in the first argument registers, uniform variadic
passing, and an initially sixteen-byte outgoing stack base. Absolute-address
programs can use all sixteen general registers. Linux FDPIC retains a module pointer
in `r15` because sharing code without an MMU is useful functionality.

Bring up the first prototype with sixteen-byte call alignment, then compare
four- and six-register preserved sets at both four- and sixteen-byte call
alignment before freezing v2. These choices interact through frame padding.
Four-byte alignment is the preferred simpler candidate if correctness and
representative absolute/FDPIC measurements support it. Use indirect transport
for all ordinary values larger than eight bytes, and do not require an
indirect-result callee to return its destination pointer. Freeze one contract
from the measurements; do not advertise a speedup percentage in advance.

## Architecture requirements

The [Epson C33 PE core manual](https://global.epson.com/products_and_drivers/semicon/pdf/id001580.pdf)
defines the following constraints. Section numbers refer to the manual, not
the PDF viewer page count. A local copy is available as `id001580.pdf` at the
repository root; use that copy for inspection rather than fetching it again.

| Hardware rule | ABI consequence | Manual section |
| --- | --- | --- |
| Sixteen 32-bit general registers and a separate SP | General-register roles are our choice | 2.1 and 2.4 |
| Little-endian memory and aligned halfword and word accesses | Define compatible layouts and handle packed data explicitly | 3 |
| SP has two fixed zero low bits | Stack alignment is at least four bytes | 2.4 |
| `call` pushes four bytes and `ret` pops them | Use this mechanism for the public ABI; private leaf conventions remain possible | 2.4.4 and 5.14.2 |
| Interrupts save PC and PSR | Preserve the hardware exception frame | 2.4.5 |
| `pushn rN` saves `r0` through `rN` | Favor a low contiguous preserved block | 2.4.2 and 2.4.3 |
| Multiply writes ALR and AHR | Model their clobbers and preserve interrupted state | 2.6 and 5.10 |
| Instructions define displacement, prefix and delay-slot limits | Code generation must respect those limits | 5.5 through 5.6 and 5.14 |

Arguments, returns, register preservation, TLS storage and a frame pointer
are software conventions. In particular, the manual does not reserve `r15`
as a data pointer or require even-numbered pairs for software 64-bit values.

## Concrete costs in the current implementation

These describe the baseline before the ABI-compatible improvements, not
application speedup measurements. The historical image counts below remain
tied to their identified binaries, rather than the newly rebuilt images.

* [The call hook](files/gcc/config/c33/c33.cc) loads a forwarding descriptor
  into `r5` at ordinary call sites for `__builtin_apply`. An installed GCC
  16.2 probe at `-O2 -mc33pe -mlong-calls` emitted two `ext` instructions and
  a load, six bytes in total, even for a wrapper that tail-jumps to another
  function. Anonymous arguments can also have both register and stack copies.
* [The allocation order](files/gcc/config/c33/c33.h) previously tried preserved registers
  as `r3, r2, r1, r0`. The prologue saves the whole block ending at the
  highest used register. A loop with one counter surviving calls chose `r3`
  and saved all four registers. The selected ABI-compatible fix prefers `r0`
  first and retains the established contiguous block-save implementation.
  An exact-set save experiment added sparse ADV/PE saves but demonstrated no
  Doom hardware speedup; it was removed to prioritize correctness and simplicity.
* Only four general registers currently preserve values across calls. FDPIC
  restoration can consume one of those registers or a stack slot for the
  incoming module base.
* `r15` remains fixed in the maintained source even in absolute-address mode.
  A comment claiming the conditional register hook frees it is ahead of the
  implementation.
* Argument placement inherits different rules for same-sized `double` and
  `long long` values, register overflow beyond the ordinary argument bank,
  and aggregate decisions based on GCC machine modes.
* The allocator's `c33_hard_regno_mode_ok` retains restrictions on multiword
  register starts that disagree with the documented unrestricted pair model.
  Audit this independently of the ABI redesign.

### Reported static image counts

The following review counts were independently reproduced with
[abi-static-counts.py](tools/abi-static-counts.py) against the identified ZIM
and Doom images and their linker maps. The script documents its static
classification rules and prints the image hash and disassembler version.

| Measure | ZIM | Doom |
| --- | --- | --- |
| `r5` descriptor loads (zero-stack-word subset), versus call instructions | 3304 (3058) vs 3143 | 2909 (2882) vs 2590 |
| Prologues that are `pushn r3` | 446 of 446 | 343 of 343 |
| Of those, saving a register the body never touches | At least 158 | At least 158 |
| Functions reading incoming stack argument words | 132 of 839; 20 skipped | 19 of 826; 8 skipped |
| Frames that exist only for sixteen-byte padding | 37 of 446 | 95 of 343 |

Reproduce from the repository root:

```sh
python3 host-tools/toolchain-c33/gcc/tools/abi-static-counts.py build/wr128/photo-progress/zim.app build/wr128/photo-progress/zim.map
python3 host-tools/toolchain-c33/gcc/tools/abi-static-counts.py build/doom/perf-render/doom.app build/doom/perf-render/doom.map
```

Both runs used `GNU objdump (GNU Binutils) 2.47.20260726`. Image SHA-256:

* `build/wr128/photo-progress/zim.app`:
  `e904c8c3538ea61ddd18394b0f937d4dbd8565d0d629973d6491784f70d6b4ad`.
* `build/doom/perf-render/doom.app`:
  `2264f77b955a232d8e3bbef078b2114f045d26f5aa3a38faac9c6c1629fe65ee`.

The descriptor recognizer counts every immediate variant with top byte
`0xc3`, and reports the zero-stack-word `0xc3000000` subset in parentheses.
Descriptors also precede tail jumps, which are excluded from the
call-instruction count, and scheduling can hoist the load away from the
transfer. The first row therefore compares two instruction counts, not the fraction of calls
carrying descriptors. This explains why Doom can have more recognized
descriptor loads than call instructions.

The other rows are classifications under the script's documented rules.
Prologue-delimited segments can include following leaf functions; the
"body never touches" lower bound assumes compiler output without implicit
register uses. Incoming-stack analysis supplements map symbols with internal
prologues, because static functions are absent from the maps, and accounts
for frame adjustments scheduled after early loads or in delay slots. The
earlier 202/350 exclusions largely reflected merged functions and scheduled
adjustments misclassified as dynamic stacks. The revised analysis skips
20 functions in ZIM and 8 in Doom, excluded from the denominators. Functions
without a recoverable boundary can still be merged; these are static
classifications rather than a complete signature census.

The padding rule scales byte and halfword accesses and rejects segments
that copy SP into another register, which can address a local indirectly.
All 37 ZIM and 95 Doom candidates allocate twelve unused bytes after
`pushn r3`. A Doom example at `0x100424de` was also inspected directly:
`sub sp,3` allocates the bytes, neither return path accesses them, and each
path restores them with `add sp,3` before `popn r3`.

These counts bound static occurrence, not execution frequency, stack traffic
or elapsed time. They support prioritizing descriptor removal and save
selection. Incoming-stack readers account for about 16% of analyzed ZIM
functions and 2% of Doom functions. In ZIM, 82 of the 132 readers access
only the first one or two stack words, making them plausible beneficiaries
of adding `r10` and `r11` to the argument bank. Confirm the transport changes
from signatures and v2 output; observed stack reads alone do not establish
that every such function will become stack-argument-free.

Padding-only allocations occur in about 8% of framed ZIM functions and
28% of framed Doom functions. Each adds a subtract and an add per invocation,
the same order of instruction overhead as the three-instruction descriptor
load per call or tail transfer. These static percentages do not establish
their relative runtime cost. The twelve-byte adjustment is required to
meet the legacy sixteen-byte outgoing base with this save block; dropping
that call-alignment guarantee requires an ABI change. Better save selection
changes the frame arithmetic without guaranteeing a reduction in padding.
The 158 candidates per image save at least one register their bodies never
touch; that does not mean all four saves are unnecessary in every candidate.
If a non-leaf needs no saves or other storage, removing an unnecessary
four-word save/reload leaves the same twelve-byte padding and `sub`/`add`.
It still removes eight word transfers. The manual specifies five base cycles
each for a four-register `pushn` and `popn`, versus two for the adjustment
pair, before memory waits (section 6.2.2 and the instruction entries).
[The emulator](../../../emulator/src/c33.c) agrees: each block transfer costs
the encoded register number plus two. Since both versions keep the adjustment
pair, removing the save/reload saves ten base cycles in this example. The
eight word transfers are already included in those base cycles; avoided
memory wait cycles can add further savings. Reducing four saves
to three instead eliminates padding. Count padding separately after save
selection is fixed; it need not fall and may rise across the revised builds.

Stack placement determines the size of that additional saving. ZIM's
[zstd sequence and four-stream Huffman decoder loops](../../../zim/zstd/zstddeclib.c)
switch through a trampoline to a private 1 KB DSTRAM stack starting at
`0x84400`, with initial SP at `0x84800`. Eliminating saves there avoids no
SDRAM stack-access waits: ten base cycles is the starting estimate, with
code-fetch effects considered separately. The rest of ZIM, including the
article converter, renderer and WebP decoder, uses the ordinary SDRAM
stack, as do Linux and NuttX. Removing the same transfers in those paths
can save several times the base cost. For scale, if each of the four reloads
incurs roughly ten additional wait cycles, avoiding those reads adds about forty cycles to
the ten-cycle base saving, before any saving on the stores. That is a
conditional sizing example, not an ISA timing guarantee or a measured v2
speedup. Controller settings, queue state, row changes and bus contention
affect the actual waits, including where a frame lies relative to the data
the function was accessing before its reload. Record stack and code
placement with benchmarks.

Measure padding in the v2 prototype, where the six-register save block
changes that arithmetic. Compare both stack alignments with both preserved
sets before the first-release freeze; do not assume their effects are
independent.
The table does not measure large-aggregate transport, so it supplies no
direct performance evidence for indirect versus direct aggregate passing.

For future baselines, retain the matching linker maps and compiler/linker
flags alongside the ELF hashes, disassembler version and counting script.
Keep classification rules explicit; do not turn "body never touches" into
a dynamic spill estimate.

Improve allocation and save selection first to establish a fair baseline.
An ABI experiment should beat the corrected existing compiler, not merely
benefit from fixes that the existing convention could also use.

## Register convention

The first candidate uses this map in both bare-metal and Linux builds.

| Registers | v2 role | Preserved across ordinary calls |
| --- | --- | --- |
| `r0` through `r5` | General registers for values that survive calls | Yes |
| `r6` through `r11` | Six argument words and ordinary scratch | No |
| `r6` and `r7` | Also scalar and small aggregate results | No |
| `r12` | Scratch and a nested function static chain when needed | No |
| `r13` | Scratch and temporary exception or lazy-binding machinery | No |
| `r14` | Scratch and indirect-call or PLT target | No |
| `r15` in absolute mode | General scratch | No |
| `r15` in FDPIC mode | Current module data base | Special restoration rule |
| SP | Stack pointer | Restored on return |
| Flags, ALR and AHR | Instruction results | No |

Prefer caller-clobbered registers for short-lived values and low-numbered
preserved registers for values crossing calls. If a function needs a frame
pointer, use a preserved register only in that function; do not globally
reserve one. Use `r0` in both candidates. A function needing only the frame
pointer can then save one register with `pushn r0`, rather than forcing a
prefix through `r5` or requiring an individual high-register save. Frame/save
lowering should still choose individual saves for other sparse sets.

Do not reserve `r12` or `r13` globally for rare language features. Express
their uses and clobbers at the affected instructions. Audit large-frame
adjustments, trampolines, PLT stubs and epilogues so none destroys a live
argument or result.

Six preserved registers may reduce repeated spills in decoders, emulators
and kernel loops. Six arguments may reduce outgoing stack traffic. Returning
in `r6` can remove copies when a result becomes the first argument of the
next call, and lets pointer-returning routines retain their first argument.
These effects depend on allocation and workload; they are not guarantees.

Compare a second candidate retaining only `r0` through `r3` as preserved,
with `r4` and `r5` available as scratch. Compare each set at both candidate
call alignments, keeping the other transport rules identical. Use separate
experimental ABI identifiers for all four combinations. Do not ship
multiple register splits or choose a different public convention per app.

## Arguments and results

Keep the current ordinary data sizes: 32-bit pointers, `int` and `long`,
64-bit `long long`, 32-bit `float`, and 64-bit `double` and `long double`.
Natural alignment is at most four bytes. Preserve existing bitfield and
record layout rules unless a separate measured change justifies replacing
them. Stronger explicit alignment attributes remain supported.

Allocate arguments in declaration order using one register cursor. One-word
values consume one slot, two-word values consume two consecutive slots,
including pairs starting at odd register numbers. Words follow memory order,
with the low word first. Floating-point values use the same bank and sizing
rules as integers; arithmetic remains software floating point.

An argument must fit wholly in the remaining six slots. Otherwise place it
wholly on the stack and close the register stream: all following arguments
go on the stack. Do not split values, overrun into `r12`, or backfill holes.
For example:

```text
f(u32, u64, u32, u32, u32)
  r6, r7:r8, r9, r10, r11

g(u32, u32, u32, u32, u32, double, u32)
  r6, r7, r8, r9, r10, stack double, stack u32
```

The second example leaves `r11` unused; the simplicity is deliberate. Measure
the occurrence and cost before adding splitting or backfilling rules.

Normalize named narrow integer arguments according to their signedness in
the caller; normalize narrow results in the callee. Default C promotions
apply to variadic arguments. Boolean results are zero or one.

Results eligible for direct transport use `r6` and, when needed, `r7`.
For indirect results the caller passes a properly aligned destination
pointer as the first argument, consuming `r6` before any `this`, VTT or
explicit arguments. No argument register is guaranteed to contain that
pointer on return; the caller retains the destination address if needed.
Ordinary scalar results still use `r6`/`r7`.

### Value classification and argument placement

Define transport by the ABI type's size and classification alignment, not
by whether GCC selects BLKmode, SImode or DImode. Classification alignment
is distinct from the storage alignment of a particular object or typedef:

* Scalars use their underlying value type's natural alignment. Alignment-only
  typedefs and object attributes do not change transport or register/stack
  cursor movement. Compatible type spellings must have identical transport
  for ordinary arguments, results and `va_arg`.
* Records/unions/classes use the alignment of their defining type and layout,
  including alignment imposed on the definition or its members. Preserve
  those definition attributes, but ignore alignment-only attributes attached
  to an alias or an object. An aligned typedef of a small ordinary record
  remains classified as that ordinary record; a genuinely aligned record
  definition retains its defining size and alignment.

Extra storage alignment remains supported when allocating actual objects
and temporaries. Reconstruct a directly transported, addressable parameter
in suitably aligned local storage if required; do not treat a four-byte
stack slot or register-save slot as sixteen-byte-aligned because of a typedef.
An alignment-only alias also cannot strengthen the alignment promised by a
transmitted indirect pointer. Use the definition-derived alignment for that
promise and honor stronger local storage requirements separately. Do not
implement classification with an unfiltered `TYPE_ALIGN(arg.type)`.

* Ordinary values of one through eight bytes, with classification alignment
  at most four bytes, use one or two words, including scalars, complex values and records.
  Copy object bytes in little-endian memory order; record padding and bits
  beyond the object have unspecified values. A three-byte record consumes
  one word. Pack and unpack only the object's bytes: never issue a misaligned
  word access or read beyond a packed source object to fill a transport word.
* Larger values or types with classification alignment above four bytes
  travel by a pointer to a properly aligned caller-owned temporary. The pointer consumes an ordinary
  argument slot. Preserve by-value language semantics: the callee can modify
  its copy without changing the caller's original object.
* For C++ classes use the established classification
  [non-trivial for the purposes of calls](https://itanium-cxx-abi.github.io/cxx-abi/abi.html#non-trivial):
  nontrivial copy/move construction or destruction, or deletion of every
  copy and move constructor, requires an invisible reference regardless of
  size. Follow the Itanium rules for construction, destruction and exception
  ownership; do not substitute a standard-library type trait for this test.
* GNU C records/unions with zero size consume no argument slot and have no
  register result or hidden result pointer. Their expressions still undergo
  normal language evaluation. A C++ empty class has its C++ size and uses the
  ordinary class rules; a one-byte class trivial for calls consumes one word.
* The same classification applies to results, with the two-word result bank.
  A small result whose classification alignment exceeds four bytes is
  indirect; alignment-only aliases do not trigger that rule. An empty C++
  class trivial for calls follows its defining size and classification
  alignment, with no blanket one-word-return exception. An ordinary one-byte
  empty class has one-word transport. Constructors and destructors retain
  their language ABI return rules. Complete and validate the C++ appendix.

The classification precedence is: language-required invisible reference,
zero-size GNU C value, then indirect transport for size above eight bytes or
classification alignment above four, then one/two direct words. C++ empty
classes receive no additional shortcut. For example:

```text
typedef long long aligned_ll __attribute__((aligned(16)));
  storage alignment 16; ABI classification alignment 4; size 8
  sink(0, x): r6 holds 0, r7:r8 hold x's value words
  va_arg(ap, long long) and va_arg(ap, aligned_ll) use the same cursor rule
  an aligned_ll result uses r6:r7, like long long

struct Small { int value; };
typedef struct Small AlignedSmall __attribute__((aligned(16)));
  alias storage alignment 16; defining classification alignment 4; size 4
  one direct word for argument/result, like struct Small

struct Empty {};
  size 1, classification alignment 1: one direct word for argument/result

struct alignas(16) E {};
  size 16, classification alignment 16: indirect argument and result
  E make_E(): caller passes destination in r6; no pointer-return guarantee
```

In particular, a sixteen-byte `_Complex double` is indirect just like a
sixteen-byte record. Wider scalar indirection and the absence of a mandatory
destination-pointer return have precedent in the
[RISC-V integer convention](https://riscv-non-isa.github.io/riscv-elf-psabi-doc/#_integer_calling_convention).
These are proposed simplifications, not measured speedups. Rebuild and verify
complex arithmetic helpers and all compiler-generated libgcc calls before
freezing them; library helper transport cannot silently retain legacy rules.

Use one placement algorithm after language conversions and classification:

1. Start the next-register index at zero (`r6`) and stack-byte offset at
   zero (relative to entry SP plus four). Include hidden arguments in their
   defined order before explicit arguments; an indirect result consumes the
   first slot. There is no even-pair rounding.
2. Ignore a zero-size GNU C value for transport. Otherwise classify its
   transport as the object itself or a four-byte pointer. Let `w` be the
   transport size rounded up to four bytes, divided by four.
3. If the register stream is open and all `w` words fit in the remaining
   six slots, place them consecutively and advance the register index by `w`.
4. Otherwise permanently close the register stream by setting its index to
   six, place all `w` words at the current stack offset, and advance that
   offset by `4*w`. Every stack slot starts at a four-byte boundary; there
   are no per-argument sixteen-byte holes. Subsequent pointers also go on
   the stack once the stream closes. This applies even to a named spill.
5. Round the total outgoing allocation to satisfy the candidate call
   alignment, placing any final padding above the argument slots. Do not
   change the first argument's entry-SP-plus-four offset.

Indirect values simplify one common argument stream, but may add copies and
pointer loads. Record dynamic argument-copy costs. Do not require a second
large-value implementation for the initial comparison. If costs are
significant in our workloads, compare direct stack copies before freezing the contract. Static
call-site counts alone cannot establish execution frequency or copying cost.
This policy does not make all large-argument copies disappear.

### Variadic functions

Use the same transport rules for named and anonymous arguments. Do not
duplicate anonymous values onto the stack and do not supply a per-call
descriptor or argument count.

Use this concrete candidate representation, independent of call alignment:

```c
typedef struct {
    unsigned int next_reg;       /* offset 0: next slot, 0..6 */
    unsigned char *reg_save;     /* offset 4: r6 slot of the save area */
    unsigned char *next_stack;   /* offset 8: next overflow slot */
} __c33_va_state;                /* size 12, alignment 4 */
typedef __c33_va_state va_list[1];
```

This is a one-element array type, not a structure typedef. A callee using
`va_start` materializes a four-byte-aligned, 24-byte register-save area in
`r6` through `r11` order. Its own frame locates that area; the caller provides
no home slots. Initialize `next_reg` and `next_stack` by applying the shared
placement algorithm to the hidden and named arguments. A named spill leaves
`next_reg == 6`; anonymous arguments must not reuse abandoned registers.

For `va_arg(T)`, classify the post-promotion type with the same ABI size and
classification-alignment rules, ignoring alignment-only aliases, and
compute `w`. If `next_reg + w <= 6`, obtain transport words from
`reg_save + 4*next_reg` and advance the index. Otherwise set the index to
six, obtain them from `next_stack`, and advance that pointer by `4*w`.
For indirect transport, load the pointer from its word and access the
pointed-to value with its required alignment. A zero-size GNU C value advances
neither cursor. `va_copy` copies all three fields into independent state;
`va_end` needs no runtime action. C default promotions and the language rules
for valid `va_arg` types still apply. Calls to variadic functions pay no
mandatory stores beyond the argument placement they actually need.

Implement this in GCC and TinyCC from the same specification and verify
calls in both directions. GCC documents the relevant lowering in its
[varargs target interface](https://gcc.gnu.org/onlinedocs/gccint/Varargs.html).

Rework `__builtin_apply_args`, `__builtin_apply` and `__builtin_return` around
the uniform incoming layout. Retain and test their supported GCC behavior;
do not retain the old compaction protocol just to accommodate different
typed and variadic layouts. Normal calls must remain free of forwarding
metadata. Define support limits explicitly rather than promising arbitrary
conversion between incompatible function signatures.

## Stack and save policy

Let `A` be the candidate call alignment, either four or sixteen bytes. SP
is aligned to `A` immediately before a public `call`. Every public C entry
must satisfy `(SP + 4) % A == 0`, with its return address at `[SP]`: twelve
modulo sixteen for `A == 16`, zero modulo four for `A == 4`. The first
prototype retains sixteen bytes; freeze the choice only after comparing
both alignments with both preserved sets. Natural object alignment remains
at most four bytes; stronger call alignment does not change type alignment.
The outgoing stack area has no mandatory register home slots. At callee entry
the return address is at `[sp]` and the ordinary first stack argument begins
at `[sp+4]`. The caller owns outgoing argument storage and restores it after
the call. No red zone is available: interrupts use the interrupted stack.

Explicitly over-aligned locals and aggregate temporaries must receive their
requested alignment through targeted frame realignment. Generated code must
preserve the original stack value for epilogues, dynamic allocation and
unwinding. Audit compiler-generated temporary alignment as well as visible
source attributes. Do not equate a smaller ABI stack guarantee with removing
alignment support.

Correct aligned objects, `alloca`, varargs and exceptions are release gates.
Four-byte ordinary call alignment is the preferred candidate for comparison;
it removes ordinary padding while explicitly aligned storage is realigned
locally. Verify that stronger-alignment compiler temporaries are covered.
The legacy padding-only counts are 37 of 446 framed ZIM functions and 95 of
343 framed Doom functions; measure invocation frequency and padding in the
v2 prototype before the alignment decision. Under sixteen-byte alignment,
with `s` saved words and no other storage, padding before another call is
`p = (3 - s) mod 4` words,
where `mod 4` gives a nonnegative remainder from zero through three. Thus
zero or four saves require twelve bytes, one or five require eight, two or
six require four, and three require none. This follows from entry SP at
twelve modulo sixteen; it is not a reason to pad a leaf that makes no call.
Actual frames also depend on local and outgoing-argument storage. Comparing
both stack policies is required before the first v2 freeze.

### Synthetic C entries

The entry invariant applies to signal handlers, fresh-thread and kernel-thread
functions, callbacks and assembly trampolines entering C. A synthetic return
address must be stored at an SP satisfying `(SP + 4) % A == 0`; aligning the
return-address slot itself to sixteen bytes is wrong for the sixteen-byte
candidate. Raw ELF/assembly startup has its own entry contract and must
establish this invariant before transferring control to C.

[Current signal delivery](../../../linux/overlay/arch/c33/kernel/signal.c)
aligns the whole signal frame to sixteen bytes and points handler SP directly
at its first return-address word, yielding zero modulo sixteen. Correct this
invariant in the implementation. Define the synthetic return slot and signal
payload alignment separately; revise the sigreturn trampoline and frame
recovery together. Verify normal and alternate stacks, nested signals,
absolute/FDPIC handler pointers, and restoration of the interrupted SP.

The 96-byte exception frame preserves the interrupted SP's residue modulo
sixteen; it does not establish C call alignment. Keep a stable frame pointer
and align the working SP to `A` before every C call, as the current common
entry does. Restore from the saved frame rather than treating the rounded SP
as its base. Cover the first entry helper, dispatch, exit and thread-start
paths, including any C helper reached before switching to the kernel stack.
Include alignment slack and helper stack use in stack-bound checks.

Save only registers a function modifies. Use a low `pushn` prefix when it
wins, individual pushes for sparse sets when they win, and shrink wrapping
where control flow and unwind information permit it. A compact instruction
that transfers six unnecessary words is not automatically faster than two
individual pushes. Update CFI for every save strategy.

## Absolute addressing and Linux FDPIC

Grifo, its bare-metal applications, NuttX, and the Linux kernel should use
absolute-address v2 code. `r15` is available to the allocator. Rewrite runtime
and assembly paths that assume a permanent `__dp` register. Keep function
pointers as code addresses in this mode.

V2 absolute code has no default data area at all. Make absolute addressing
mandatory, reject explicit `-mno-edda32` or non-FDPIC `-msep-data`, and do not
silently accept objects whose addressing assumes a persistent `r15` base.
The compiler and assembler must agree on this mode; just freeing the register
while retaining the legacy addressing option is incorrect.

Check relocations as well as ABI flags at assembly and link time. Absolute
v2 objects must not contain `R_C33_DH`, `R_C33_DL`, `R_C33_DPH`, `R_C33_DPM`
or `R_C33_DPL`, nor the legacy `GL`, `SH/SL`, `TH/TL` or `ZH/ZL` data-area
relocation families. Reject the corresponding relocation expressions, target
symbol/section attributes and data-area directives. Under v2, reject legacy
`sda`, `tda`, `zda` attributes and `.gcomm`, `.scomm`, `.tcomm`, `.zcomm`
directives. Ordinary ELF `.comm`, `.data` and `.bss` remain available.
Do not reject native ELF TLS sections such as `.tdata` and `.tbss` by name:
they are a different facility from the legacy target data areas.

Linux userspace retains FDPIC: code can be shared between processes while
each module's data is separately placed. Keep the current descriptor format
of an entry address plus the defining module's `r15` base. Keep `r15` reserved
in this mode and preserve its explicit restoration after calls that can
enter another module. Known local calls can avoid that restoration.

FDPIC v2 deliberately retains the default module-base relocation forms it
uses today. Apply the prohibition above to absolute objects; do not ban all
data-base relocations merely because an object is v2. Reject the legacy
auxiliary data areas in both modes. Before freezing the relocation whitelist,
audit every supported FDPIC producer and runtime path.

Do not try to free `r15` by simply marking it caller-clobbered while still
emitting implicit data-base accesses. [Existing FDPIC lowering](ABI.md#-mfdpic-shared-libraries)
depends on its value. Nor does declaring it callee-saved solve cross-module
calls: the dispatch path changes the base before entering the callee.

Update PLT and lazy resolver code for v2 arguments, results and preservation.
Retain correct descriptors, relocations, constructors, TLS, exceptions and
dynamic loading. Keep the existing restriction on tail calls across modules
until the restoration contract supports them; optimize local tail calls.
Treat alternative FDPIC restoration schemes as later experiments.

Continue Linux's existing thread-pointer RAM slot rather than taking a
general register. Preserve its scheduling and TLS publication guarantees.
Reclaiming `r15` is an absolute-mode optimization; removing native Linux TLS
or shared libraries is outside this proposal.

An allocatable but preserved `r15` in absolute mode is a reasonable later
candidate if profiles show pressure on values surviving calls. It adds an
isolated save/restore when modified; never save `r0` through `r15` just to
preserve it. It would also require saving `r15` in absolute-mode context
switches. This does not make absolute and FDPIC allocation identical:
FDPIC still reserves the register, changes it before cross-module entry,
and requires caller restoration. Retain scratch `r15` as the initial
absolute-mode policy rather than assuming a common register number makes
those costs disappear.

## Operating system and service boundaries

Use explicit trap interfaces independent of private kernel C functions.

| Interface | Proposed v2 wire convention |
| --- | --- |
| Grifo services | `int 1`; service number in `r12`; arguments in `r6` through `r11`; result in `r6` or `r6:r7` |
| Linux syscalls | `int 0`; number in `r12`; six raw argument words in `r6` through `r11`; result in `r6` |
| NuttX internal services | Use its configured OS mechanism with v2 wrappers; do not inherit Grifo's service dispatcher |

Use `r12` for both Grifo and Linux service numbers so new trap dispatchers do
not decode inline data at the saved PC. Keep service numbers and source API
names where useful, while changing the binary result and preservation
convention deliberately. Loading a number above the short-immediate range
can make a Grifo stub larger than the old halfword encoding. The unextended
two-byte load covers -32 through 31; loading the larger Grifo service numbers
needs a prefix, so the load plus trap takes six bytes instead of the old
four-byte trap plus halfword. These sizes exclude the common return
instruction. Record that tradeoff rather than claiming the register encoding
is always smaller.
Grifo services with more argument words use the ordinary v2 stack rules;
the dispatcher must present those stack arguments correctly to the C
service. New service wrappers and callback entry points must use v2
consistently. Declare `r12` clobbered by service wrappers as for an ordinary
call; inline trap sequences must model it explicitly. The compiler handles
any value that needs to survive the invocation.

For Grifo, also prototype a versioned service call table provided at startup.
Absolute v2 applications and Grifo share an address space and ordinary C
convention, so wrappers could call or tail-jump through code pointers without
the software-interrupt frame and trap-to-C dispatch. The table must identify
its ABI and supported entries; keep kernel addresses out of application
link-time assumptions. Preserve callback, interrupt-state, stack-argument
and service semantics. Measure representative file, timer and event calls
before replacing the trap path. This is an implementation experiment for
Grifo, not a replacement for Linux's syscall entry, kernel-stack handling,
tracing or signal machinery. Retain the legacy service-16 adapter only where
needed during migration.

Moving the Linux syscall number out of `r4` avoids modifying a v2 preserved
register. Keep Linux syscall numbers and negative-error semantics. Rewrite
libc syscall and cancellation wrappers, clone/vfork startup, signal return,
ptrace, audit, seccomp and restart handling together. Save the original
syscall number and first argument explicitly in the kernel frame; the old
`orig_r4` convention is no longer sufficient. Preserve six raw words for
restart, including 64-bit syscall arguments.

Replace [the current synthetic syscall function signature](../../../linux/overlay/arch/c33/kernel/irq.c),
which accommodates four C argument slots and the `r9:r10` exception, with
proper typed wrappers from the raw syscall frame. Six C slots simplify
transport but do not justify incompatible function-pointer casts or ignoring
64-bit argument reconstruction.

Interrupts and signals must preserve the entire interrupted machine state,
including caller-clobbered registers, ALR, AHR and flags. Voluntary context
switches may rely on the selected v2 preserved set; the six-register
candidate must save `r4` and `r5` as well. Update task creation and initial
frames to match. Caller-clobbered does not mean an interrupt may discard the register.

Lay out the general-register block in `pushn` memory order: `r0` at the
lowest address, followed by `r1` through `r15`. Linux's current `r[16]`
array already has this order; the optimization is arranging entry and exit
to use block transfers instead of individual extended stores and loads.
Place software metadata and special-register saves around the block so the
hardware PSR/PC pair remains valid for `reti`.

Save all general registers in each vector stub before loading its vector
number. With six software metadata words, a compatible entry shape is:

```text
sub   %sp,6          ; reserve 24 bytes above the general-register block
pushn %r15          ; save 64 bytes, r0 first in memory
xld.w %r4,vector     ; the interrupted r4 is already saved
xjp   common_entry
```

The hardware has already pushed eight bytes. This sequence produces a
96-byte frame with registers at offsets 0 through 60, software metadata at
64 through 84, PSR at 88 and PC at 92. The core pushes PC first, then PSR;
`reti` reads PSR at the lower address and PC four bytes above it. This order
matches the hardware-pair offsets in the current `pt_regs` and return shim.
Define the six software words as follows:

| Byte offset | v2 field |
| --- | --- |
| 64 | ALR |
| 68 | AHR |
| 72 | Interrupted SP |
| 76 | `vector_and_flags` |
| 80 | Original syscall number, or `0xffffffff` outside a syscall |
| 84 | Original first syscall argument, or zero outside a syscall |

`vector_and_flags` bits 0 through 7 hold the vector, bit 8 is
`C33_FRAME_FROM_USER` (`0x100`), and bits 9 through 31 are zero in v2.
Dispatch and diagnostics extract the vector with `& 0xff`; `user_mode()`
tests only `& 0x100`. Set the origin from the entry bookkeeping before
changing `current->thread.in_kernel`, not from the interrupt-enable bit in
PSR. Each nested frame records its own origin: an exception in a kernel
handler has the flag clear even when the outer frame came from userspace.
Keep the outer frame's flag intact. Signal restoration and fresh user
contexts set it explicitly rather than accepting a flag from a user-provided
signal context.

The existing field called `reserved` is not spare:
[user_mode()](../../../linux/overlay/arch/c33/include/asm/ptrace.h) tests it and
[entry/exit bookkeeping](../../../linux/overlay/arch/c33/kernel/process.c)
sets it. Packing its origin information into the vector word leaves room
for both original syscall fields without enlarging the 96-byte frame.
Update UAPI/debug/core consumers deliberately and generate assembly offsets
from this representation; do not silently reuse old field interpretations.

[The current ptrace setter](../../../linux/overlay/arch/c33/kernel/ptrace.c)
copies directly into the entire `pt_regs`; v2 must not retain that behavior.
Ordinary general-register writes, including partial writes, preserve the
entire kernel-owned `vector_and_flags` word and the syscall-bookkeeping
words. Ignore incoming bytes for those fields. Stage the input, validate
the writable registers and allowed PSR bits, then commit only documented
writable state; a failed write leaves the frame unchanged. A register
snapshot restored by a debugger must not change the vector or origin.

Define intentional syscall edits through a separate syscall-edit interface
at syscall stops. It may change the original number (including `-1` to skip
a syscall) and original first argument; these are not side effects of a
general-register restore. At entry, number/first-argument edits also update
live `r12`/`r6` for dispatch. At exit, writing the result in `r6` leaves the
original first argument unchanged; an explicit restart-argument edit updates
that saved argument separately. Document the interface and restart semantics
before implementing ptrace, and keep the vector/origin word read-only there
too.

The block save needs no repair of an overwritten register slot. An assembler
probe with vector 255 produced fourteen bytes for this sequence, within the
sixteen-byte vector-stub slot.
Verify all vectors and the final linked jump range during implementation.

Signal delivery and exec can change the return SP, so the return shim still
needs explicit treatment rather than a blind `popn` followed by `reti`.
Update assembly offsets, ptrace, signal and core layouts together. Block
transfers still move the saved register words, but remove instruction-fetch
traffic: the current individual GPR stores and loads occupy well over thirty
bytes on each path, versus two bytes for a `pushn` or `popn`. That distinction
matters for SDRAM-resident entry code. Frame setup, special-register handling
and any partial restore remain additional instructions; measure complete
entry and exit sequences rather than claiming a two-byte total interrupt
handler or a data-transfer speedup from code size alone.

Update setjmp/longjmp and unwind implementations for the new preserved set,
stack policy and results. Keep exception pointer/selector in `r6`/`r7` and
the temporary unwind adjustment in `r13`, with no permanent reservation.
Retain DWARF unwinding for Linux. Evaluate replacing bare-metal GCC's SJLJ
exception mode with DWARF separately so the ABI comparison does not conflate
exception implementation changes with register changes.

## Factory boot compatibility

Retain ELF32 C33 `ET_EXEC`, machine number 107, the PE core identifier, section
headers the old loader understands, and the current linked memory placement.
The [factory-style loader](../../../samo-lib/drivers/src/elf32.c) loads sections
and calls `e_entry` with one old-ABI argument. It does not need to understand
ordinary calls inside the new image. Its source ignores `e_flags`, but this
must also be verified against the actual preserved factory image.

Replace the fragile C entry code in Grifo with assembly startup: accept the
legacy boot argument in `r6`, disable interrupts, establish our stack and
runtime state, and enter v2 C. Never return to the flash loader. Keep the
old memory reservations and required clock/device setup; an ABI change does
not justify bypassing the real boot chain.

During migration, new OS images may still be launched by legacy Grifo.
Provide a small assembly entry accepting old `argc`/`argv` in `r6`/`r7`,
and retain the nonreturning service-16 exit adapter. Both interfaces work
without imposing the legacy ordinary-call ABI on the operating system.
Once Grifo and the launcher are rebuilt, they use v2 internally and when
launching rebuilt applications.

Rebuild ZIM, Game Boy, Doom, the launcher and supported old applications.
Do not build a general mixed-ABI object ecosystem or require every new app
to run on untouched 2009 Grifo. Old card images remain a separate recovery
option. Keep adapters only for the factory boundary and specific retained
legacy services or artifacts that are actually needed.

## Object identification and build isolation

Introduce an explicit `-mabi=legacy|v2` compiler/assembler selection, an ABI
ELF flag in currently unallocated bits, and a compiler macro for v2. Allocate
the bits after auditing existing core and FDPIC flags. BFD must reject
legacy/v2 mixtures, including archive members and partial links. Distinguish
all four experimental register/alignment combinations until one is selected. Runtime loaders
we control must reject incompatible applications and shared objects.

The flag checker must also enforce each data mode's relocation and target
attribute policy. An absolute-v2 flag on an object with default-data-area
relocations must fail, including in archives and partial links. Check inputs
before final relocation removes that evidence. Raw numerical assembly can
still express an ordinary register-relative access; a relocation checker
cannot infer whether it wrongly assumes a permanent base. Audit maintained
assembly and require producers to select the correct ABI/data mode rather
than treating a flag as proof of arbitrary machine code's semantics.

An ABI flag prevents an invalid link; it does not translate an interface.
Legacy boundary adapters are v2 assembly objects whose external contract is
explicitly documented, rather than unmarked old C objects admitted through
the linker. Never set the v2 flag on an unchanged legacy binary.

Use separate build trees, compiler installations, sysroots, library archives
and generated headers. Build identities must include compiler, ABI and data
mode so Make cannot reuse stale objects. Rebuild libgcc and its soft-float
helpers before testing programs; matching frontend code with stale runtime
archives can silently corrupt results.

## Implementation scope

| Component | Required work |
| --- | --- |
| GCC and binutils | Calling hooks, register classes, allocation/save costs, arguments, varargs, results, calls, frame lowering, CFI, ELF checks and libgcc |
| Grifo and mini-libc | Assembly startup, service dispatch/stubs, callbacks, vectors, suspend, memory/string/division routines and setjmp |
| Bare-metal applications | Rebuild all code and audit hard-coded register roles; update Game Boy's generated hot assembly and renderer, Doom's math, and Mini vMac's native emitter |
| NuttX | Startup, task/context switches, IRQ/signal frames, setjmp, libc assembly and module/runtime boundaries |
| TinyCC | Calls, register/result model, variadic handling, aggregates, frame generation, ABI flags and its in-memory linker |
| Linux and uClibc | Kernel assembly, typed syscall wrappers, signal/startup contracts, NPTL, TLS, cancellation, setjmp, dynamic resolver and unwind integration |
| Tooling | Debugger/core register descriptions, disassembly metadata, emulator fixtures, ABI probes and build identity checks |

TinyCC currently hard-codes scratch and frame registers in
[its generator](../../../nuttx/overlay/tinycc/c33-gen.c). Mini vMac
[emits saves and helper calls directly](../../../minivmac/cfg/WRM68KJIT.h),
and Game Boy [generates its hot assembly](../../../gameboy/gen-hot.py).
Changing GCC alone would leave these producers using the wrong contracts.
Give generated code a defined private convention and v2 adapters, or update
it to v2. Consider private helper conventions only after profiles show a
benefit; keep public libraries and function pointers on the common ABI.

Tune register and memory costs in GCC so it can exploit the new map. Evaluate
LTO and interprocedural register analysis for private calls after the ABI
baseline is stable. Known private helpers can need fewer spills than an
unknown external call, without creating another public convention. Keep
these experiments separate from the initial ABI comparison.

Keep a register-return-address leaf convention available as a separate
private experiment. The local manual, section 5.14.2, explicitly demonstrates:

The device reads the PC this way correctly after every delayed branch
(`emulator/tools/pc_read_test`, 2026-10-02):

```text
jp.d leaf
ld.w %r8,%pc        ; delay slot captures the continuation
; continuation

leaf:
    ...
    jp %r8
```

The PC read must be in the delayed jump's slot, as the manual specifies.
The example's `r8` is not a proposed fixed link register: it is an argument
register in v2. Choose a register that the private target leaves intact and
that does not conflict with arguments. Start with compiler-controlled,
proven leaf clones whose addresses do not escape; their private entry SP
and return lowering differ from ordinary public functions. Preserve ordinary
function-pointer calls and public `call`/`ret` entry points. Account for
register pressure, the occupied delay slot, stack layout, DWARF return-address
rules, debugging and asynchronous unwinding before using it. It can avoid
the return-address stack write/read, but is not a first-release requirement
or an established speedup.

## Measurements and release gates

Keep a corrected legacy build as the baseline, then compare v2 candidates
with the same inputs, optimization flags, linker settings, hardware clocks,
IRAM budget and emulator model. Record code alignment because changes can
alter fetch-queue behavior; repeat close results with controlled layouts.

Evaluate this matrix before freezing the register/stack contract:

| Preserved registers | Call alignment |
| --- | --- |
| `r0` through `r3` | Four bytes |
| `r0` through `r3` | Sixteen bytes |
| `r0` through `r5` | Four bytes |
| `r0` through `r5` | Sixteen bytes |

Keep argument/result/varargs rules identical across those builds. Start with
a small working GCC prototype and early ZIM/Game Boy/Doom measurements,
then include representative Linux/FDPIC workloads before the final choice.
In particular, measure module-base restoration and dynamic calls alongside
padding, spills and context-switch costs. Prefer four-byte call alignment
when the results do not justify a stronger public guarantee; explicitly
aligned objects still require correctness at either alignment.

| Workload | Primary observations |
| --- | --- |
| ZIM | Article/text decode and WebP rendering CPU time; cold and warm loading separately; resulting pixels and file correctness |
| Game Boy | Fixed scripted frames and gameplay states; slowest windows and missed deadlines; reference frame/memory hashes |
| Doom | Fixed demo/gameplay frames, rendering/conversion phases and frame times; separate boot and card I/O |
| NuttX | Context switches, libc, interpreter work, TinyCC selfhost and OS regression suite |
| Linux | CPU microbenchmarks, libc/soft-float, process/thread creation, syscall loops, dynamic calls, C++ unwinding, plus boot and interactive workloads |

Collect text/rodata size, stack high-water marks, dynamic save/spill traffic,
padding-only frame counts and execution frequency, calls, argument/result
moves and cross-module restoration costs alongside elapsed time. More
allocatable registers is not a sufficient outcome.
Distinguish CPU improvements from SD throughput, waiting, application frame
caps and hardware setup. Use the full factory-FLASH to Grifo to launcher
path for application tests and performance runs; bare ELF execution remains
appropriate for isolated compiler tests only.

Run existing compiler and differential suites for each candidate. Add
independent GCC/TinyCC cross-calls and small assembly callers/callees covering
every argument-bank boundary, odd pairs, signed narrow values, small records, indirect and aligned records,
complex values, indirect results with no returned pointer, and mixed variadic
arguments. The assembly cases must independently check the specified word
placement; two compilers agreeing is not enough. Include packed three-,
five- and seven-byte objects, misaligned source/destination addresses,
boundary guards for overreads, zero-size GNU C records, C++ empty classes,
and over-aligned small results. Include `struct alignas(16) E {};` in both
argument and result examples, expecting indirect transport. Cross-call
compatible declarations using plain and alignment-only typedef spellings
for scalars and records, including direct/indirect results, register and stack spills, and
varargs retrieved through either compatible spelling. Check addressable
aligned parameter copies and temporaries separately from transport.
Exercise named spills before `va_start`,
anonymous spills that leave one register unused, indirect variadic values,
`va_copy` independence and passing the array-form `va_list` to helpers.

Check SP at the first instruction of public C entries, before a compiler
prologue hides its original residue. Cover synthetic signal entries, nested
and alternate-stack handlers, fresh user/kernel threads, callbacks and
every trap-to-C call for both alignment candidates. Verify origin flags for
user, kernel and nested exceptions and after exec, signal return and ptrace.
Ptrace tests must attempt full and partial writes over the vector/origin
word and verify it is unchanged, including forged origin and vector values.
Check that ordinary register restores preserve syscall bookkeeping, explicit
syscall edits support replacement/skip/restart, exit-result writes do not
overwrite the original argument, and failed writes commit no partial state.
Include register canaries, setjmp, dynamic stacks, exceptions,
interrupts/signals arriving with all
registers live, TLS across switches, and FDPIC calls and lazy binding.
Negative link/load tests must reject wrong ABI objects rather than silently
execute them. Add negative absolute-v2 cases for forbidden data-area
relocations and attributes, and positive FDPIC/TLS cases to ensure the
restrictions do not reject legitimate module-relative or thread-local data.

Release requires correct outputs and all relevant existing regressions,
repeatable improvement on representative CPU work without an unexplained
material regression elsewhere, and stack/code size within board budgets.
Confirm performance on physical hardware for representative workloads;
emulator-only results do not establish hardware speedups. If changing the
register/stack combination fails that gate, retain the simpler v2
argument/varargs rules and select the measured combination that meets it.

## Build sequence

1. Bank and validate the ABI-compatible allocation/save and signal-entry
   fixes. Use their measured baseline before deciding whether to prototype
   v2. Audit
   unused `r15` reservation and multiword allocation restrictions; keep
   findings separate from changes that require rebuilding interfaces.
2. Complete a reviewable candidate psABI and its argument-placement algorithm
   before implementing it independently in both compilers. Resolve the
   language ABI appendix, stack-slot rounding, aligned and empty results,
   concrete `va_list`, origin-bit representation and synthetic entry rules;
   include assembly examples as independent transport oracles. Add ABI
   identification and isolated v2 GCC/binutils/libgcc builds. Bring up the
   first sixteen-byte GCC prototype, then its four-byte variant, with
   complex helper and varargs checks before booting an application.
3. Implement absolute v2 Grifo, mini-libc and assembly entry; rebuild the
   launcher, ZIM, Game Boy and Doom as a coherent card. Boot through the
   actual unchanged factory flash and run application correctness checks.
   Start application measurements of all four register/alignment candidates
   here; keep the prototypes small enough to correct early findings.
4. Port NuttX and TinyCC, including native-generated code. Validate GCC/TinyCC
   cross-calls and NuttX task, interrupt and interpreter paths.
5. Port Linux kernel, uClibc/NPTL, FDPIC loader and C++ runtime together.
   Validate signals, restart, ptrace, TLS, dynamic calls and unwinding.
6. Finish the four-way register/alignment comparison with representative
   FDPIC results before freezing the first release. Keep indirect large-value
   transport fixed for that comparison; revisit it if measured copy costs
   warrant a separate experiment. Evaluate the Grifo call table and private
   leaf-call convention separately. Select one contract, document every
   transport rule and finalize its identifier before making v2 the default.

The deliverable is a written psABI, matching GCC and TinyCC implementations,
matching runtimes and generated assembly, an unchanged factory boot path,
and a benchmark report against the corrected baseline. The historical
compiler remains useful as an ISA/legacy-boundary oracle, not the authority
for v2 ordinary calls.
