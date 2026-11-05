# Matching RISC-V calibration

The four original `riscv/rv*-device.txt` placement reports were captured on
2026-09-15. Today's interpreter excludes console time and has a different
divide implementation and dispatch layout. `riscv/README.md` already records
these changes, but `fit-model.py` still used the old numbers as a timing
oracle. Identical guest instruction counts do not make those cycle totals
comparable. The old score is now an explicit `--historical` diagnostic.

Build a matching four-variant set:

```sh
python3 emulator/tools/riscv_bench/build.py
python3 emulator/tools/riscv_bench/run.py
```

Builds live under `build/wr128/rv-calibration`, each in its own source/object
directory; existing RISC-V application builds are preserved. `manifest.json`
records every application hash and the source/guest/reference hashes. The
runner boots FLASH -> Grifo -> launcher -> SDRAM interpreter -> A0 code ->
A0 code/state -> assembly hot path, and checks all ten guest checksums in
each saved report. All four verified in the emulator before installation.

The optional `calibrate` argument makes an application chain to the next
variant after closing its report. The last one powers off. Console capture
is inside the existing untimed console region, and the capture is appended
to the card report after measurement, so hardware results include checksums
without needing a serial cable. Ordinary RISC-V runs retain their usual
report format and termination behavior.

The installed launcher entry is:

```ini
rvcal.ico : rvsdr.app rvbench.bin rvsdr.txt calibrate
```

Run this entry once, then return the card to the Mac after it powers off.
The collector finds it itself:

```sh
python3 emulator/tools/riscv_bench/collect.py
python3 emulator/tools/score_model.py --suites riscv
```

The collector verifies all installed inputs against `rvcal.json`, requires
all four complete reports and native-reference checksums, and preserves the
reports and their hashes alongside `device.json`. The scoring command
requires the same build manifest, guest image, Grifo and launcher as the
hardware run. It also compares per-kernel instruction counts and checksums.
Missing fresh data leaves qualification incomplete; historical tables are
never substituted silently.
