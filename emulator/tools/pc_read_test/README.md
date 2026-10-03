# What `ld.w %rd,%pc` reads

`pctest.app` is a Grifo application that reads the PC with `ld.w %rd,%pc`
the ways the PE manual allows and the way it does not, and compares each
read with the address the manual gives. The manual (section 5.14.2, and the
caution on printed page 119) defines the read only in a delayed branch's
slot, as the address of the instruction after the slot. GCC's
nested-function trampoline (`jp.d .+4; ld.w %r12,%pc`) depends on it.

Each kind of read runs at sixteen alignments from SDRAM and two from A0
RAM, 1,000 times each:

| Kind | The read sits in the slot of |
| --- | --- |
| `jp.d-next` | `jp.d` to the instruction after the slot, as GCC's trampoline does |
| `jp.d-far` | `jp.d` further on, so the next instruction and the target differ |
| `jp.d-reg` | `jp.d %rb` |
| `jrne.d-taken` | a conditional branch that is taken |
| `jrne.d-not` | one that is not |
| `call.d` | a call, to a stub that returns |
| `ret.d` | a return (in SDRAM and in A0 RAM) |
| `plain` | nothing: an ordinary read, which the manual does not allow |

It then calls a real GCC trampoline, built on the stack by taking a nested
function's address, 700 times.

## Device results

`device-2026-10-02.txt` is a 32 MiB reader. Every delayed form read the
address after its slot every time: 126,000 reads, no misses. The
trampoline returned the right value 700 times in 700. The plain read never
gave the PC; it gave whatever the previous instruction left behind, most
often the value of the register the padding moves wrote (`0x5a5a5a5a`).
wremu does the same for the delayed forms and rejects the plain one.

## Running it

```sh
python3 emulator/tools/pc_read_test/run.py
python3 emulator/tools/pc_read_test/run.py \
  --device emulator/tools/pc_read_test/device-2026-10-02.txt
```

`run.py` boots wremu through the launcher (MBR flash, Grifo, `init.app`,
`pctest.app off noplain`) and reads `pctest.log` back from the card; wremu
stops at a plain read, so `noplain` skips that kind.

On the device, copy `build/wr128/pc-read/pctest.app` and `pctest.ico` to
the card's root and add `pctest.ico : pctest.app` to `init.ini`. Tap the
icon; the run takes a few seconds, writes `pctest.log` and returns to the
launcher at a tap.
