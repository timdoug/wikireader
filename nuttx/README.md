# NuttX on the WikiReader

`nuttx.app` is a launcher application, like `zim.app` and `doom.app`: the icon
menu starts it. What runs then is Apache NuttX on the S1C33E07, with the
240x208 panel as a scrolling terminal, the touch panel as a keyboard, `vi`, and
a native TinyCC that compiles C33 code on the device — including itself. NSH's
`poweroff` turns the device off and `reboot` restarts it. There is no way back
to the menu without restarting.

It is the odd one out among the applications here. The other two are Grifo
programs: they call the kernel for files, the display and events, and they
leave the machine to it. NuttX takes the machine over instead — its own trap
table, its own interrupt controller state, its own scheduler — and keeps it
until the device stops. Grifo stays resident below `0x10040000`, untouched,
which is the only reason `poweroff` and `reboot` can work at all: cutting the
power rail and arming the reset watchdog are board specifics it already
implements, and NuttX asks it rather than repeating them.

The card is NuttX's too. It reads the slot with its own SPI and MMC/SD
drivers rather than asking Grifo, and mounts the boot partition at `/sd`, so
work saved from `vi` or built with `tcc` outlives the session. See "The card"
below.

## What you get at the prompt

NSH's own built-ins, which is most of a shell already: `ls cat cp mv rm mkdir
rmdir df du free ps top mount echo test expr printf hexdump xd sleep usleep
time uname date kill pidof pkill source exec set unset export env basename
dirname pwd cmp truncate uptime watch dmesg`, plus `if`/`while`, pipes,
redirection, command substitution, background, aliases, shell variables
(`CONFIG_NSH_VARS`) and command history.

On top of those:

| | |
| --- | --- |
| `vi` | full-screen editing on the panel — see the board README |
| `tcc` | compiles C33 code on the device, itself included |
| `hexed` | hex editor |
| `stty` | terminal settings |
| `cpuload` | per-task CPU share, sampled from the scheduler tick |
| `sz` / `rz` | ZMODEM over UART0 — the only way files leave this device |
| `mb` `mh` `mw` | peek and poke memory by byte, halfword and word |
| `lua` | Lua 5.4 with its standard library, 32-bit numbers |
| `wrforth` | the Forth this device shipped with — see "The Forth" below |
| Toybox | 139 commands under their own names: `awk grep sed find sort wc head tail xargs cut tr uniq od xxd diff tar gzip seq stat md5sum dd tee more patch readelf` ... |

`mb`/`mh`/`mw` are worth knowing about on this board: `mw 0x00300660` reads a
hardware register from the shell, which beats rebuilding to find out what one
holds.

`sz`/`rz` transfer on `/dev/console` and land files in `/tmp`; they were the
only way anything left this device before the card was readable.

`nuttx.app` is 2,488,664 bytes, against 1,561,040 before any of this: 76 KB
for the tools above, 38 KB for the card, 180 KB for the two interpreters, the
rest for Toybox.

### Lua

`CONFIG_INTERPRETERS_LUA`, which wanted nothing that was not already here:
`ARCH_SETJMP_H`, `SYSTEM_SYSTEM` and `LIBC_LOCALE` all went in for Toybox, and
`LIBM` and `SYSTEM_READLINE` were already on, so the REPL has line editing.

Two things had to be fixed to make it more than a banner. `nuttx_linit.c`
registered `luaopen_base` and nothing else, so `math`, `string`, `table`, `io`
and `os` were all nil in an interpreter that started, printed its version and
evaluated arithmetic perfectly well — while its Makefile compiled every one of
those openers under the same option and the linker then dropped them for want
of a reference. And that Makefile tested `CONFIG_INTERPRETERS_LUA_32BITS`
where the Kconfig defines `..._32BIT`, so the 32-bit number model the option
defaults to had never once taken effect. Both are in `patches/apps.patch`.

32-bit is worth having here. Measured on the device with `time`:

| | integers | floats |
| --- | --- | --- |
| 32-bit integers and floats | 0.98 s | 1.15 s |
| 64-bit integers and doubles | 2.14 s | 3.24 s |

for a hundred thousand `s = s + i % 7` and fifty thousand `s = s + 1.0 / i`.
There is no floating-point unit either way, so the second row is paying for
software doubles and 64-bit loop counters. The cost is `math.maxinteger` of
2³¹-1 and seven digits of precision.

One thing to know: `2^10 == 1024` is **false**. NuttX's own libm computes
`powf` as `expf(e * logf(b))`, which lands about two units in the last place
away from an exact power; `print(2^10)` rounds to `1024.0` and hides it.
`math.sqrt` is exact.

### The Forth

```
nsh> wrforth

moko forth interpreter for S1C33 (build:23)
 Ok
include /sd/cli.4th
ls
```

This is not a new Forth: it is the one the WikiReader shipped with, the
Openmoko eForth in `samo-lib/forth`, running as a task. The `.4th` files on a
stock card are its programs, and they are *source* — `calc.4mu` is three lines
that say `include calc.4th` and then run `calculator` — so making them work
meant making the interpreter read the card, not porting each one.

Everything it knows about the world outside its dictionary goes through 57 C
calls, which is small enough to answer twice. `apps/interpreters/wrforth`
answers them with POSIX: `FileSystem_*` become `fopen`/`fread`/`opendir`,
`Serial_*` become the task's own standard input and output. The rest — the
buttons, the touch panel, the battery, the serial FLASH — answer as absent
rather than being removed, so a program that asks gets a defined answer and
not a link error. The LCD words need no C at all: they write to `0x00080000`,
which is where the framebuffer still is.

The interpreter is metacompiled by `gforth` at build time, so `gforth` and
`gawk` have to be on the path. Three changes let the same sources build both
ways, all guarded by `NUTTX_FORTH` so the bare-metal image is unchanged:

* the entry point. `main` sets up the machine — trap table, interrupt
  controller, drivers, and a cleared status register that would turn
  interrupts off for everyone. `forth_entry` sets up the two Forth stacks and
  starts the interpreter, and nothing else. Those stacks are still the
  interpreter's own 64 KB pair in `.bss` rather than the task's, because
  `cold` resets the return stack pointer there in any case — so only one
  Forth task can exist at a time.
* the dictionary. On the metal it grows into whatever SDRAM is left after the
  image; here it gets `.forth_heap`, 256 KB of `@nobits` that costs nothing on
  the card, and `cold-cp0` points at it.
* the dictionary's labels stay local. Forth has words called `exit`, `abort`
  and `abs`, and sharing a namespace with a C library does not end well.

#### Every immediate word had lost its flag

Worth writing down, because the failure was so quiet. The metacompiler cannot
know a word is immediate until it reads the `immediate` after the closing
`;`, so it writes the dictionary header first and the flags afterwards:

```
	COLON	forth_dict "\073" semicolon flags_semicolon
	...
	END_COLON
flags_semicolon = 0
flags_semicolon = flags_semicolon + FLAG_IMMEDIATE
```

The assembler this was written against resolved that forward reference to the
finished value. A current one takes the first definition instead, so every
flags word assembles as zero and nothing in the dictionary is immediate. The
image still builds, still starts, still prints its banner, still evaluates
`1 2 + .` — and then `;` gets compiled into the definition it was meant to
close, the interpreter never leaves compile state, and from the first colon
definition onwards every line is silently swallowed. `hoist-flags.awk` gathers
the flags and emits them above the headers. It fixes the bare-metal build too,
which had the same hole.

`REGBIT` in `trailer.s` had a related problem: it used the same name for the
Forth constant and the assembler symbol, so 313 of them failed to assemble at
all once the assembler stopped allowing a label to be redefined.

### Toybox: the rest of the POSIX set

Toybox supplies 139 commands, and they work under their own names — `awk`,
`sed`, `find`, `sort`, `wc`, `xargs`, `diff`, `tar`, `gzip`, `od`, `xxd`,
`cut`, `tr`, `uniq`, `seq`, `stat`, `strings`, `md5sum`, `more`, `patch`,
`readelf` and the rest — with pipes, redirection and everything else the
shell does:

```sh
seq 5 | sort -r | head -2
find /tmp -type f -exec wc -c {} ';'
sort -n data | awk 'NR==1 {print "min:", $0}'
```

There is no standalone `find`, `sed` or `awk` in nuttx-apps; this is the only
route to them. Its `grep` also replaces the standalone NuttX one, which knows
six options against Toybox's twenty-eight; `dd` and `tee` come from Toybox for
the same reason, that having both would mean two `dd_main` symbols (see below).

Getting the plain names took a change to NSH itself, in `patches/apps.patch`.
Toybox is one binary behind one entry point; elsewhere a symlink per command
makes each reachable by name, and there is no file system here to hold those.
Nor can each be registered as a NuttX builtin, since that wants an entry-point
symbol per command and a multi-call binary by construction has one. So NSH
gains `CONFIG_NSH_MULTICALL`: a command that is neither a shell command nor a
registered builtin is retried as `toybox <command>` before being called
unknown. It costs one table lookup on a command that was going to fail anyway,
nothing can be shadowed because both other lookups come first, and redirection
is already applied to the task's descriptors by then, so pipelines work.

It cost more than a Kconfig flip, because the board it is documented against
is `sim`, built on a Linux host — so everything the host quietly supplies had
to be found and dealt with:

* `<sys/syscall.h>` needed `arch/c33/include/syscall.h`, and `<setjmp.h>` an
  actual implementation; both are below.
* `<paths.h>` does not exist here. Upstream already defines the one macro
  toybox uses from it for NuttX but left the `#include` (patch `0024`).
* **`vfork()` does not exist on this architecture**, and toybox's process
  spawning is built on it. Only 7 of NuttX's 18 architectures have it, and
  `SYSTEM_TOYBOX`'s Kconfig does not say it depends on one. Rather than write
  ~500 lines of stack-copying assembly for a board nobody has powered on,
  patch `0025` spawns instead — see "How processes start" below.
* `getdelim()` and `sysconf(_SC_ARG_MAX)` were both wrong in NuttX's libc;
  see "Three bugs found on the way".

Two are deliberately off: `setsid` wants session leadership, which NuttX does
not have, and `getconf` wants POSIX limit macros NuttX does not define.

The command set is `Kconfig.commands`, which upstream keeps by hand and
describes as "curated to applets known to build on NuttX" — 112 of Toybox's
317. Adding to it is a config block per applet and then finding out; that is
where the 27 beyond the original list came from, `awk` among them, which
needed patch `0027` for two names NuttX had already taken.

**One implementation per name.** The standalone NuttX `grep`, `dd` and `tee`
apps are disabled, because this is a flat build and every `*_main` is an
ordinary global symbol:

> With both enabled the linker binds Toybox's dispatcher to the NuttX app's
> `grep_main`, which takes `(argc, argv)` while Toybox calls applets with
> none and passes arguments in `toys.optargs`. It read the argument registers
> as a pointer and went off into unmapped memory. Anything named the same in
> both does this, silently — check
> `nm staging/libapps.a | grep ' T .*_main' | sort | uniq -d` after a *clean*
> build when adding an app whose name Toybox also uses; stale archive members
> give false positives.

Building it needs the network once (a pinned tarball) and GNU patch;
`make nuttx` finds `gpatch` if Homebrew has it and falls back to Apple's,
which applies this series fine. Note that toybox's Makefile only defines
`clean` when it is enabled, so a tree that has had it on and then off cannot
be cleaned until it is turned back on.

### How processes start

There is no `fork()` here, and none is wanted: NuttX creates a task at an
entry point, in one address space with no MMU, so nothing is copied and a
child never runs the parent's code. `task_spawn()` takes a *function
pointer*, and the descriptors a Unix program would set up for itself between
`fork()` and `exec()` travel with the spawn instead, as
`posix_spawn_file_actions`. NSH uses exactly this for its own pipes and
redirections.

Patch `0025` rewrites toybox's `xpopen_setup()` that way, plus the three
commands that reach for `vfork()` directly (`time`, `xargs`, and `setsid`,
which is simply off). A toybox applet is not a NuttX builtin of its own —
they all live behind the single `toybox` one — so the spawn prepends that
where needed, and anything else is looked up in NuttX's builtin table, which
is what keeps `find -exec nsh` working.

`xargs`, `find -exec` and `time` are all verified to spawn and wait
correctly, as are pipelines — which needed the state fix below, since both
halves of a pipeline are Toybox commands running at once. What is lost is `xpopen_setup()`'s callback, whose only user is
`timeout`'s `setpgid()`: the child is no longer this process, so there is
nowhere to run it. `timeout` still works; its child stays in this process
group. `find -execdir` is rejected rather than silently wrong, because it
needs `fchdir()`, which NuttX implements by asking a descriptor for its path
— something no filesystem here answers.

### Diagnostics

Mostly unavailable, and for the same kind of reason. Of the five, only
`cpuload` needs nothing from the architecture. `taskset` wants SMP, which is
one core here; `stackmonitor` wants `ARCH_HAVE_STACKCHECK` and stack
coloration; `dumpstack` wants `up_backtrace()`; `critmon` wants
`up_perf_gettime()`. None of those exist for C33.

### Toybox state is per task

Toybox keeps everything a command needs in globals — `toys`, `this`, and the
two scratch buffers — which is exactly right when every command is its own
process. This is a flat build: one address space, and a "process" is a task
sharing it with every other. Two commands alive at once wrote over each
other, which is not a corner case at all. It is both halves of a pipeline,
and it is `find -exec` or `xargs` running a command while the parent walks
its tree. It also failed quietly rather than loudly:

```
seq 3 | tac                       faulted on a half-overwritten list
find dir -type f -exec wc -c {} ; counted the first file, then printed
                                  bare names for the other two
```

Patch `0026` moves them into per-task storage, allocated on first use and
freed by a task-local-storage destructor at task exit (`CONFIG_TLS_TASK_NELEM`).
They stay ordinary struct members rather than becoming pointers, so
`sizeof(toybuf)` and `&this` still mean what every user of them expects, and
the names are unchanged, so no command source needed touching. The cost is a
function call and a TLS lookup per access, and about 9 KB per task that
actually runs a command.

### The C library's own tests

`apps/testing/libc` builds eight suites here, and they are worth having given
how the bugs below were found — by accident, one at a time, while porting
something else:

```sh
python3 boards/c33/s1c33e07/wikireader/tools/test_libc.py
```

`arch_libctest`, `atomic`, `fmemopen_test`, `fopencookie_test`,
`open_memstream_test`, `popen_test`, `scanftest` and `wcstombs`: 196 checks,
all passing. Each is also an ordinary command at the device's own prompt, so
a run here and a run on hardware compare the same thing.

`arch_libctest` is the interesting one: it covers `memcpy`, `memmove`,
`memset`, `memcmp` and `strlen` — this port's hand-written C33 assembly — at
eight alignments and every boundary size, against a byte-loop oracle.

It did not, however, cover the bug we had just fixed. Every one of its
character-search cases looked for `0x7e`, so `strchr` searching for a byte
with the high bit set was untested, which is exactly where the fault was.
`patches/apps.patch` adds a high-byte case to the `strchr`, `strrchr` and
`strchrnul` tests; reverting the libc fix makes it fail at every alignment
and size, which is the only way to know a regression test is worth anything.

`stdbit` wants C23's `<stdbit.h>`, which this NuttX does not have, and
`crctest` wants the CMocka framework; both stay off. `scanftest` needs a
writable scratch path — `CONFIG_TESTING_SCANFTEST_FNAME` points at `/tmp`
here rather than its default `/mnt/fs`.

### Eight bugs found on the way

Five were latent in NuttX itself -- three in the C library, which Toybox is
the first thing here to exercise that much of, and two in the FAT driver,
found by the card work below; they are fixed in `patches/nuttx.patch`. The
others are Toybox's, in patches `0025` and `0028`.

Three of the six are the same mistake: assuming plain `char` is unsigned. It
is signed here — `DEFAULT_SIGNED_CHAR 1` in the compiler's C33 backend,
because the chip's byte load sign-extends — and C leaves that choice to the
implementation, so a byte of `0xff` read into a `char` compares equal to `-1`
and not to `255`. Worth suspecting first when something on this target
silently returns the wrong answer. Note that only NuttX's `strchr` family is
target-specific in any real sense: it is invisible where `char` is unsigned,
as on ARM. Both bc bugs reproduce on any signed-char platform, this
development Mac included.

**`getdelim()` believed a size it had no business reading.** With
`*lineptr == NULL` there is no buffer for `*n` to describe, and callers
routinely pass an uninitialized size alongside — glibc and musl both ignore
it. NuttX took the value at face value and tried to allocate a
garbage-sized buffer, failing the call. A caller checking only for `> 0`
reads that as end of file, which is why `sort` and `sed` printed nothing at
all while `cat` and `wc` were fine: the line-reading path was returning "no
lines" every time.

**`sysconf(_SC_ARG_MAX)` was unimplemented**, falling through to `ENOSYS`
and `-1`. `xargs` sizes its batches with it and concluded every command was
too long.

**`ARG_MAX` is the POSIX minimum of 4096 here**, so even implemented,
toybox's `ARG_MAX - environ - 4096` leaves nothing. Patch `0025` floors that
at a usable value, which is a fix for any small-`ARG_MAX` platform, not just
this one.

**`bc` had two of its own**, both `char`-is-unsigned assumptions, both silent
(patch `0028`). Its keyword table packs a POSIX flag into the top bit of a
`char` and the keyword's length into the rest; signed, that makes the length
mask sign-extend to about four billion, `strncmp()` never matches, and every
POSIX keyword — `scale`, `if`, `while`, `for`, `define`, `sqrt` — quietly
stopped being a keyword and lexed as an ordinary variable. That one bug
explained all of it: `scale=2` assigned a variable that happened to be called
`scale` while the interpreter's own scale stayed 0, so `1/2` answered `0`;
`bc -l` could not parse its own math library, which is written with `define`
and `return`; and `while` and `if` did nothing. The second: variable names in
its bytecode end with an `0xff` byte read back as `c != UCHAR_MAX` with `c` a
`char`, so `-1 != 255` was always true and every lookup used the wrong name.

**`strchr()`, `strrchr()` and `strchrnul()` never matched a byte over 0x7f.**
All three compared `*s == c` without converting `c` to `char` first, as C
requires. Where `char` is signed — as it is here — a byte with the high bit
set promotes to a negative `int` and never equals the positive `c` the caller
passed, so the search silently found nothing. `memchr()` beside them gets it
right, which is what the three were measured against. Found through `bc`,
which terminates the variable names in its bytecode with `0xff` and looks for
them with `strchr()`; it died on the first expression because the search
returned NULL and the pointer arithmetic after it underflowed into a
negative allocation size.

**FAT could not open a file whose name filled the 8.3 form.** Two mistakes in
one line of `fat_path2dirname()`, both from measuring a name against
`DIR_MAXFNAME`, the width of the name field in a directory entry. That field
is eleven bytes and holds no dot, but written out an eight-plus-three name is
twelve characters and wants a thirteenth byte for a terminator. So
`contrast.4th` was too long for the length test and skipped the short-name
parse entirely, and `payload.bin` passed the test but was copied into an
eleven-byte buffer with nothing to end it, leaving the parser reading the
stack. Either way the short name came out empty and the entry never matched.
Both kinds of file list perfectly and then cannot be opened, which is what a
card full of `.4th` and `.bin` files looks like. Only reachable with
`CONFIG_FAT_LFN`; the short-name-only build takes a different path.

### Two gaps closed on the way

`setjmp()` and `longjmp()` are now part of the architecture
(`arch/c33/include/setjmp.h`, `arch/c33/src/common/c33_setjmp.S`,
`CONFIG_ARCH_HAVE_SETJMP`), saving the ABI's four callee-saved registers, the
caller's stack pointer and the return address. NuttX's own `ostest` has a
`setjmp` case gated on exactly this, and it passes, including `longjmp(env, 0)`
returning 1. The TinyCC application carries a private copy predating this and
can drop it.

`arch/c33/include/syscall.h` is the empty one every architecture has to have,
because `<sys/syscall.h>` includes it unconditionally. This is a flat build
with no system call interface, so there is nothing in it.

## The card

```
nsh> mount
  /proc type procfs
  /sd type vfat
  /tmp type tmpfs
```

The chip has one SPI master, and both the card and the serial FLASH hang off
it behind their own chip selects on port 5. `arch/c33/src/s1c33e07_spi.c`
drives it as a NuttX SPI bus; the board file owns the parts the controller
knows nothing about -- the chip selects, and the card's supply, which comes up
as rail-off, then rail, then buffer, because raising the buffer into an
unpowered card back-feeds it. Above that are NuttX's stock `mmcsd_spi` and
`vfat`, and the MBR reader, since a WikiReader card is partitioned: a small
FAT32 boot volume and a much larger exFAT one. Each partition is registered as
`/dev/mmcsd0N` and the first is mounted.

The exFAT partition stays unreadable -- NuttX's FAT driver does not do exFAT.
`zim.app` reads it through Grifo, which does.

A block does not go a byte at a time. High-speed DMA channel 3 drains the
receive register into the buffer while channel 2 refills the transmit register
from a fixed word of ones, and because that register empties when the shifter
takes its contents rather than when it finishes with them, the next word is
already queued while the current one is on the wire. The CPU writes one word
to start it and then waits. Characters are 32 bits wide for this, which is the
whole point: 128 transfers to a sector instead of 512. Measured in the
emulator, against the same run reading the same card:

| | MCLK cycles per 512-byte block |
| --- | --- |
| CPU, a byte at a time | 29,670 |
| DMA, byte units | 36,822 |
| DMA, 32-bit units | 16,892 |

The wire itself is 16,384 of those cycles at MCLK/4, so the last row is within
3% of what the clock allows, and the middle row is why this is not simply "use
DMA": at one request per byte the engine setup costs more than the CPU loop it
replaces.

Three things about this are the board's and not the manual's, and all three
come from the Grifo driver beside it, which was measured on hardware:

* A 32-bit character arrives most significant byte first, so each word has to
  be reversed after the transfer.
* Changing the character width needs an enable cycle, and an enable cycle with
  the card selected costs it a bit of whatever it is shifting out. The clock
  pin is parked as a GPIO held at the idle level while the width changes.
* The engines write to SDRAM dependably and to the internal RAMs less so, so a
  buffer below `CONFIG_RAM_START` takes the CPU path, as do short or
  misaligned ones.

A transfer that stalls is not retried: what arrived is finished by the CPU,
and DMA stays off for the rest of the session, because an engine that stopped
early has already left the card mid-block.

The descriptor and the transmit word live in the chip's 2 KB descriptor RAM at
`0x00084000`, which is where Grifo keeps its own -- harmless, since Grifo is
not running.

`wremu` models all of this, including the SPI bit rate, the DMA engines' bus
timing and the card's block timing, so the numbers above are the emulator's
model of the device rather than host time.

## Layout

The port is about 11,000 lines spread over three upstream projects. Carrying
their trees here would add some 340 MB to every clone, so this directory holds
the port and nothing else:

```
revisions           the three upstream repositories and the exact revisions
overlay/<name>/     files that do not exist upstream -- the port itself
patches/<name>.patch  diffs against those revisions for files that do
fetch.sh            clone the revisions into work/ and lay the port over them
update-port.sh      the reverse: record work/ back into overlay/ and patches/
work/               the reconstructed, buildable trees (git-ignored)
```

`overlay/` is ordinary source: `arch/c33` (the architecture port), the board
under `boards/c33/s1c33e07/wikireader`, `libs/libc/machine/c33` (word-at-a-time
string routines in assembly), `apps/interpreters/tinycc`, and TinyCC's C33 code
generator. `patches/` is the small change to existing upstream files:
registering the architecture in Kconfig, NXTerm's VT100 parsing and batched
rendering, the packed-framebuffer fixes, and the PTY's bulk writes.

## Building

```sh
make nuttx
```

from the repository root, or `make` in this directory. The first build clones
NuttX, nuttx-apps and TinyCC into `work/` — about 340 MB and the only step that
needs a network. Everything after that is local. The result is `nuttx.app`
here, stripped, and `work/nuttx/nuttx` with its symbols kept for debugging.

Besides the C33 toolchain, the build needs GNU `make` and `flock`
(`brew install make flock`) and Python. `fetch.sh` creates a private virtual
environment in `work/nuttx/.venv` for kconfiglib and `kconfig-tweak`, so
nothing is installed system-wide.

Nothing else in the tree depends on this: `make`, `make grifo`, `make zim` and
`make doom` never reach it, and it does not need them either — it links against
none of the kernel. The dependency only runs the other way, and only at run
time: putting `nuttx.app` on a card needs Grifo and the launcher, and `make
test` skips if they have not been built.

`make clean` removes the build products and keeps the configuration, like the
other applications; `make distclean` drops the configuration too, and `make
realclean` the fetched upstream trees with it. The repository's own `make
clean` reaches only the first of those.

Besides the C33 toolchain this needs `gforth` and `gawk`, which metacompile
the Forth, and a network for the pinned Toybox and Lua tarballs.

`CONFIG` picks the board configuration; it defaults to `app`, the one this
directory exists to build. `lcd`, `tcc`, `nsh` and `ostest` are for a loader
that hands over the whole machine and never wants it back, and are documented
in `work/nuttx/boards/c33/s1c33e07/wikireader/README.md` along with their
tests.

## Putting it on a card

```sh
make grifo zim nuttx
zim/make-card-image --nuttx card.dmg archive.zim
```

That adds `nuttx.app` and `nuttx.ico` to the boot volume and a third line to
`init.ini`, after the ZIM reader and the stock reader, so the menu offers all
three. The application needs nothing else on the card.

## Changing it

Edit in `work/nuttx`, build and test there, then:

```sh
make -C nuttx update-port
```

which sorts every change into `overlay/` or `patches/` by whether the file
exists upstream, and leaves the result for review with `git diff`. `make -C
nuttx diff` shows what has not been recorded yet.

Updating to a newer upstream is the same two steps in the other order: bump
the revision in `revisions`, run `./fetch.sh --force`, fix whatever the patch
could not apply, and run `update-port.sh`.

## How the handoff works

Grifo's loader reads the ELF's section headers, places every allocated section
at its linked address, zeroes the `NOBITS` ones, and jumps to `e_entry` with
interrupts enabled. There is no special application format and nothing to link
against: `wikireader:app` only has to be an `ET_EXEC` C33 image that stays
above `0x10040000`, which `CONFIG_RAM_START` arranges.

Coming in, NuttX's `_start` clears `PSR.IE` on its first instruction, so
nothing of Grifo's runs once the jump is taken. `up_irqinitialize()` keeps a
copy of the trap table, then clears the interrupt controller and installs
NuttX's own. `c33_lowsetup()` stops the watchdog — Grifo arms a 20-second reset
before calling an application, and NuttX would never poke it.

Going out is nine instructions in `wikireader_grifo.c`: disable interrupts,
restore that trap table, and `int 1` with the halfword `16` after it, which is
Grifo's `System_exit`, with `EXIT_POWER_OFF` or `EXIT_REBOOT` in `%r6`.

Nothing else is handed back, because nothing else can matter. `System_exit`
runs to `power_off()` or the reset watchdog with interrupts disabled — Grifo
only ever restores an interrupt state it saved, never enables unconditionally —
so no interrupt can be delivered between the trap and the end, and the
interrupt controller can be left exactly as NuttX had it.

That is worth spelling out because it was not true of the first version of
this, which returned to the icon menu with `EXIT_RESTART_INIT`. Grifo reloads
the launcher off the card with interrupts on, and NuttX's timer and serial
sources were still enabled in a controller whose table no longer had handlers
for them, so it panicked. Coming back needed the enable registers saved at
startup and restored in the right order, a stack switch because the reload runs
on the caller's stack, and the frame buffer blanked because the launcher draws
over whatever is there. `poweroff` meaning power off costs none of that.

## Testing

```sh
make -C nuttx test
```

boots the emulator through Grifo from a generated card, taps the first icon,
runs `uname` and `free`, and leaves with `poweroff`; then does it again with
`reboot`. `--compile` adds compiling and running a C file on the device, which
costs about a billion emulated cycles.

It checks what the device did, not only what was asked for. wremu models the
power rail Grifo toggles and the reset its watchdog asserts, so `poweroff` has
to end the run with `stop reason: powered off` and stay down, and `reboot` has
to reset the machine and bring the launcher all the way back up. Grifo's
printed exit code — 1 and 2 — is checked too, since that is the argument
surviving the trap.

`test_sdcard.py` runs next: it builds a partitioned FAT32 card, checks that
the guest's `md5sum` of a 64 KB file matches the digest computed here, has the
guest write a file and unmount, and then reads that file back out of the raw
image rather than believing what the guest said about it. It also fails if
nothing came in over DMA or if a transfer stalled into the CPU path, so a
silent fallback shows up as a failure and not merely as a slower run.

`test_lua.py` and `test_wrforth.py` follow. The first runs `check.lua` off a
card and fails if any library is missing, which is the failure that matters
for an interpreter that starts either way. The second types a Forth session
and then `include`s `cli.4th` — this repository's own program, the one a stock
card carries — which opens directories and files, reads lines, writes, renames
and deletes, and so exercises most of the interpreter's C calls at once. It
also checks that a colon definition actually takes effect, because a Forth
whose immediate words have lost their flags passes every simpler test.

`test_libc.py` beside it runs NuttX's own C library suites; see "The C
library's own tests" above.

The port's own tests — the 215-tap terminal regression, the compiler and
self-hosting runs, the ASan/UBSan host tests for the graphics, terminal, PTY
and touch code — live in the board's `tools/` directory and are described in
its README. They exercise the same code through a direct ELF boot or the
FLASH/card loader, and are the right place to look when something breaks that
is not the handoff itself.

## What is missing

Everything the port's own README lists, and, specific to running as an
application:

* **No exFAT.** The card's second partition, which is where a ZIM archive
  lives, is registered as `/dev/mmcsd0p2` and cannot be mounted: NuttX's FAT
  driver does FAT12/16/32 only. ChaN's FatFs would read it.
* **No card detect.** Nothing reports the slot's state, so `SPI_STATUS` says
  the card is present and a missing one shows up as an identification
  timeout at boot. Swapping cards while running is not handled at all.
* **No buttons, no power management.** The front buttons are not bound, and
  the device does not suspend. The idle loop halts, which is most of the
  benefit, but a NuttX session will flatten the batteries faster than the
  reader does.
* **Untested on hardware.** Every result here is from `wremu`. The emulator
  models the SDRAM timing, the watchdog and the power rail, but not the
  panel's electrical behaviour or power draw. The handoff in particular deserves a device before
  it is trusted: it depends on Grifo's resident state being exactly as the
  emulator leaves it.
