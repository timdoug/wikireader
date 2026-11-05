# Variable-unroll local-port probe

The earlier timing loops all repeat eight body copies per branch. This probe
varies that count to distinguish access throughput from loop restart costs,
while preserving the frozen pipeline and matched-dependency applications.

It measures 20 shapes at unroll counts 1, 2, 3, 4, 8, 12 and 16, A0 code
offsets 0 and 2 and A0/SDRAM data: 560 cases, each with three 20,000-pass
samples. Shapes include NOP/register controls, prefixes, reads, stores,
dependent/independent XOR consumers and mixed work. Every loop retains the
same short branch encoding. Build-time symbol/size checks reject relaxation
or unexpected layout; the largest template is 250 bytes in an owned 512-byte
buffer. SDRAM code is excluded to isolate the local instruction/data port.

Input is 13 with initial R10=5. The returned R10 XOR R11 checksum observes
consumers too, so a stale first load consumed as 5 can no longer hide behind
the correct final load result. Actual results and guards are checked every
repetition. Each log row records the actual checksum, bytes and unroll.

```sh
python3 emulator/tools/port_bench/build.py
python3 emulator/tools/port_bench/run.py --out build/wr128/portq/my-validation
python3 -m unittest discover -s emulator/tools/port_bench -p 'test_*.py'
```

The runner snapshots the emulator and boots actual FLASH -> Grifo -> real
init.app -> portq.app, preserving input hashes and native reports. The first
validation exposed a host heuristic that stopped at nine valid NOPs. After
removing it, all 560 cases pass in `build/wr128/portq/validation-nop-fixed`.
This validates emulator execution, not silicon timing. The subsequent
`build/wr128/dma-write-review/engine-portq` run also passes all 560 cases on
bc75d10a; its full report is byte-identical. These validations preceded the
physical return described below.

For physical collection, install `portq.app` and `portq.ico` with the launcher
entry `portq.ico : portq.app off`, using ordinary filesystem copies and a
backup of any replaced entry/files/log. Preserve other menu entries. Installed
`portq.json` must contain the build manifest and SHA-256 identities under
`files` for `portq.app`, `portq.ico`, `kernel.elf`, `init.app` and `init.ini`.
The collector discovers the mounted volume itself; no raw image is needed.

```sh
python3 emulator/tools/port_bench/collect.py
python3 emulator/tools/port_bench/analyze.py --run build/wr128/portq/my-validation --out build/wr128/portq/hardware-comparison.json
```

Collection preserves raw failed native reports before validation and marks
`device.json` invalid, preventing silent use of an older successful return.
Comparison requires matching application/boot identities, clock/address
context, workload coverage and native results. It reports raw cycles/pass,
all signed case errors, sample vectors and adjacent-unroll slopes. Slopes
are diagnostic; no fitted overhead is subtracted from the accuracy numbers.

The validated installer is `install.py`; it verifies the known external
volume, archived reader evidence and matching validated boot/application
inputs before copying. Tests cover backups, menu preservation, changed input
rejection before writes and collection using the installed identities.

The physical installation is complete. The user additionally authorized
cleanup: 29 obsolete hardware-probe files were backed up locally and removed.
The card now keeps five normal apps and portq, the rightmost icon on the
second row. All copied inputs, normal apps and cleanup were verified after
remount, then the card was ejected. Installation/backup/cleanup identities
are in `build/wr128/portq/installed.json`. The installer accepts the earlier
reader-probe layout; it is not a generic reinstaller for this cleaned menu.
Collect the returned report without asking the user for its path.

## Returned physical evidence

The physical run is archived in `device.json` and
`device-2026-11-05-13425df38ae4.txt`. All 560 cases and installed identities
pass. The frozen prospective files are unchanged from before collection.

`build/wr128/portq/prospective-hardware-comparison.json` evaluates all six
declarative hypotheses without rerunning or refitting them. Eight raw
halfwords reaches 496/560 within 10%: 217/280 shared-A0 and 279/280 separate-
port cases. The four-halfword reference reaches 491/560. Neither reaches
the shared-port target. All 56 matched dependent/independent contrasts have
median separation at most 0.0003 MCLK/pair, opposing a generic load penalty.

Actual executing-C33 replays on the qualified 8dafbeab core are separate:
default timing reaches 208/560 within 10%; the optional frontend reaches
489/560 (218/280 shared A0, 271/280 separate port). The declarative forecast
uses simplified external memory costs. Large remaining errors concentrate
in short shared-port loops. The raw comparisons are under
`build/wr128/portq/returned-current-*-comparison.json`.

```sh
python3 emulator/tools/experiments/compare_port_predictions.py --out build/wr128/portq/prospective-hardware-comparison.json
```

After collection, portq's files and menu were backed up and replaced by
fetchq. The five ordinary apps and six menu positions were verified after
remount, then the card was ejected. The subsequent fetchq run has also
returned and is archived under `../fetch_bench`. Both reports are collected;
the card is ejected, with fetchq retained as its only diagnostic entry.
