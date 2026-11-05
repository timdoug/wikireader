# Matched load dependency and bus probe

This extends the frozen pipeline probe without changing its source or binary.
The 44 sequences run from SDRAM and two A0 halfword alignments, against A0
and SDRAM data: 264 cases, three raw samples, 20,000 passes and eight copies
per pass. The controls change only the register read after a load; the
dependent and independent instruction streams have identical sizes and
instruction counts. XOR, compare and move consumers, halfword/word loads,
zero to two intervening moves, overwrites, adjacent loads and extended
immediate controls help separate dependency effects from fetch/data sharing.

```sh
python3 emulator/tools/bus_bench/build.py
python3 emulator/tools/bus_bench/run.py
python3 emulator/tools/bus_bench/test_collect.py
```

The build generates `build/wr128/buspipe/buspipe.c`, application, icon and a
manifest. The runner boots FLASH -> Grifo -> real launcher -> application;
it requires every expected case, all return values and memory guards, three
positive samples per case, and `END BUSPIPE cases=264 failures=0`.

For hardware, install `buspipe.app`, `buspipe.ico` and an installation identity
file `buspipe.json` alongside this launcher entry:

```ini
buspipe.ico : buspipe.app
```

The installation identity contains the build `manifest` and a `files` map
of SHA-256 hashes for the application, icon, kernel, launcher and `init.ini`.
Archive any previous `buspipe.log` before installing. Start the entry, wait
for Done, then power off and return the card. Collection finds the card
automatically and rejects changed inputs or incomplete/failed runs:

```sh
python3 emulator/tools/bus_bench/collect.py
python3 emulator/tools/bus_bench/analyze.py --run /path/to/emulator-run --out build/wr128/buspipe/comparison.json
```

It preserves each report under a date and content hash, with its identities,
and updates `device.json` to point to the latest verified hardware run.
Compare paired median tick differences divided by `20000 * 8` to measure
the effect per load/consumer pair. Keep alignment and both memory placements
separate. A small difference alone does not prove a generic CPU interlock.
The analyzer verifies hardware/emulator build and boot identities, clock
registers, memory placements and code sizes. Its observed ranges span the
extremes of the three samples; they are descriptive, not confidence intervals.

The returned 2026-11-05 report is archived under `device.json`: all 264 cases
and installed inputs pass. In 64 of 66 contrasts the median difference is
less than 0.0001 MCLK/pair in magnitude. The two exceptions (SDRAM code,
`word_use2` versus `w_indep2`, both data placements) differ by 0.9553
MCLK/pair. Disassembly places the independent loop wholly inside one 1-KiB
row, at 0x10040760--0x100407a2; the dependent loop crosses 0x10040800,
running from 0x100407c0 through its branch at 0x10040802. All corresponding
A0 contrasts are zero. Treat these two as placement-confounded, rather than
evidence of a load dependency; the precise row/fetch cost remains unresolved.
The current production comparison is `build/wr128/buspipe/current-comparison.json`.
