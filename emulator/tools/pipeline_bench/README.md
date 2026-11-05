# C33 pipeline probe

This Grifo launcher application separates prefix compression, load result
forwarding and fetch alignment. It runs 20 instruction sequences from SDRAM
and copies of the identical bytes at all eight halfword offsets in an A0
16-byte line. Each uses A0 and SDRAM data: 360 cases, three raw samples each,
20,000 passes/sample and eight copies of the sequence per pass.

The sequences include register moves, single/double prefixes before ALU,
load, store and taken-branch instructions, prefixes after a load or a move,
halfword/word load dependencies with zero to two independent instructions
between load and use, and a mixed constant/load/ALU sequence resembling
the interpreter. Each loop returns a checked value; guards and data words
are checked after every sample. No card I/O, serial output, watchdog kicks,
or interrupt handlers run inside the measured interval. Timer-call and
function-entry overhead remain in the raw ticks; use the matched controls
and divide their differences by `passes * 8` for a per-sequence cost.

The application owns its A0 code buffer and data through ELF `.fastbss`.
It does not borrow the framebuffer, IVRAM window or kernel scratch memory.
Templates contain no absolute code references. The cross assembler resolves
their local relative branches before copying; the same instructions run
in every code placement. `pipeline.log` records programmed clock/memory
registers, addresses, code sizes, raw samples, expected return values and
verification status. Every case closes the log before starting the next.

Build and check through FLASH, Grifo and the launcher:

```sh
make -C emulator/tools/pipeline_bench
python3 emulator/tools/pipeline_bench/run.py --no-build
python3 emulator/tools/pipeline_bench/run.py --no-build --device /path/to/pipeline.log
```

On hardware, copy `build/wr128/pipeline/pipeline.app` and `pipeline.ico`
to the card root and append this entry to `init.ini`:

```ini
pipeline.ico : pipeline.app
```

Start it from the launcher. When it displays “Done”, tap to return, power
the reader off and return the card to the Mac. Collect `pipeline.log` from
the boot partition. The final line must be
`END PIPELINE cases=360 failures=0`. The emulator runner uses `off` as the
application argument to power down automatically after the log is closed.

The initial emulator validation passed all 360 cases.
Keep this exact app binary for hardware comparisons: changing code layout
changes the very effect the probe is measuring.

## Hardware results, 2026-11-05

The physical run in `device-2026-11-05.txt` passed all 360 cases. The adjacent
JSON records the log, application, Grifo and launcher SHA-256 hashes; the
application on the returned card matched the tested emulator binary's app.
Programmed clocks and SDRAM timings match, except for read-only refresh
status bits. Each figure below is the median sample divided by 20,000;
timer/entry overhead is included.

| SDRAM code, A0 data | Device cycles/pass | Old model | With A0/SDRAM overlap |
| --- | ---: | ---: | ---: |
| Eight register moves | 13.032 | 14.034 | 14.034 |
| Eight halfword loads | 17.032 | 22.034 | 18.034 |
| Eight word loads | 21.032 | 26.033 | 22.033 |
| Eight word stores | 13.032 | 18.034 | 14.034 |

Subtracting the move control and adding its one cycle per instruction gives
1.5 cycles/halfword load, 2/word load and 1/store. The older model charges
2, 2.5 and 1.5 in both code placements. Half an MCLK of local data overhead
is now hidden when the fetch stream uses SDRAM. This is an empirical overlap
model; the measurements do not identify the internal circuit that causes it.
The remaining common cycle per pass is in loop control/fetch timing.

Other observations remain separate from the implemented change:

- From A0 code, eight single-prefix AND pairs take 14.029 cycles/pass
  against the move control's 13.029. A prefix is usually compressed, with
  an extra cycle at the loop restart. Eight double-prefix pairs take
  21.029 or 22.029 depending on halfword alignment. Always charging every
  prefix one cycle is wrong; always making them free is also wrong.
- A0-code/A0-data halfword loads take 23.029 or 24.029 cycles/pass depending
  on alignment; word loads take 27.029 and stores 19.029. The current model
  retains its older local-code calibration while that fetch/data interaction
  is investigated.
- Immediate halfword-load/XOR pairs take 37.029 or 38.029 cycles/pass;
  inserting one independent move gives 41.029, and two gives 45.029 or
  46.029. A single flat load-use charge cannot describe both the latency
  hiding and the alignment effect.

Historical-suite comparisons must also account for refresh phase. Changes
to boot costs can move a short timed copy relative to refresh without
changing the copy instructions. The validation artifacts include a paired
memory-benchmark sweep across seven boot-card cluster sizes; its raw
measurements and binary identities are in `build/wr128/pipeline/phase-sweep/`.
