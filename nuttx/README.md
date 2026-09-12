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
| Toybox | 139 commands under their own names: `awk grep sed find sort wc head tail xargs cut tr uniq od xxd diff tar gzip seq stat md5sum dd tee more patch readelf` ... |

`mb`/`mh`/`mw` are worth knowing about on this board: `mw 0x00300660` reads a
hardware register from the shell, which beats rebuilding to find out what one
holds.

`sz`/`rz` transfer on `/dev/console` and land files in `/tmp`; they were the
only way anything left this device before the card was readable.

`nuttx.app` is 2,419,572 bytes, against 1,561,040 before any of this: 76 KB
for the tools above, 38 KB for the card, 111 KB for Lua, the rest for
Toybox.

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

Besides the C33 toolchain this needs a network, for the pinned Toybox and Lua
tarballs.

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

`test_lua.py` follows: it runs `check.lua` off a card and fails if any
library is missing, which is the failure that matters for an interpreter that
starts either way.

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
