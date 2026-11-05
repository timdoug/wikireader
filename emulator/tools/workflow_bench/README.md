# Reader timing measurements

This reuses the reader's existing benchmark driver, retrieval instrumentation
and renderer. `bench LABEL calibrate` adds three repeated lookup/render runs
for Simple English Cat and Tokyo and Wikivoyage Paris, four viewport steps
per article, a checked completion marker, and automatic power-off. The
original selected archive is restored after measurement.

```sh
python3 emulator/tools/workflow_bench/build.py
python3 emulator/tools/workflow_bench/prepare.py --out build/wr128/workflow-calibration/my-fixture
python3 emulator/tools/workflow_bench/run.py --fixture build/wr128/workflow-calibration/my-fixture --out build/wr128/workflow-calibration/my-run
make -C emulator test-score-model
```

The build lives in a separate reader directory under `build/wr128`; ordinary
reader outputs are preserved. Preparation uses macOS `hdiutil` and an APFS
clone of the existing `build/wr-card.dmg` (or `--source IMAGE`). It mounts only
that clone under a private temporary directory and opens no raw block
devices. It verifies Grifo/launcher identities, records full archive hashes
and the complete prepared image hash, installs the calibration application
and list, enables existing startup instrumentation, and detaches the image.

The runner verifies and clones the fixture, snapshots the emulator, creates
the actual FLASH image and boots FLASH -> Grifo -> `init.app` -> reader.
It preserves commands, environment, binary/input/report hashes, the written
card image, raw reports and per-phase sample vectors. `--collect-only` can
revalidate an existing run from its saved `inputs.json`; it does not boot.

## What is measured

- Lookup uses the same prefix search and exact-title resolution as the
  existing benchmark. It excludes typing, keyboard repaint and debounce.
- Existing retrieval, article-open, first-paint and first-screen-finished
  boundaries remain. The raw MCLK ticks are retained alongside the older
  benchmark's microsecond summary.
- Four fixed viewport requests move by +104, +104, -104 and -104 pixels
  through the renderer's active-gesture mode, including direct framebuffer
  scrolling and its sub-window scrollbar. Real touch parsing and coasting
  are outside this probe.
- Checksums cover all 240x208 displayed bitmap pixels, compositing the LCD
  sub-window. Hashing, logging and formatting occur outside timed intervals.
- Lookup/retrieval I/O calls, sectors and read ticks remain separate from
  total time. Read time is a subset of elapsed time, not an added cost.
- Existing `zimboot.log` records application-to-keyboard phases and kernel
  file-load windows. A run contains one cold boot, explicitly counted as
  one; three article repetitions do not establish repeated boot timing.

Article/blob caches are dropped before lookup and again before retrieval.
Filesystem caches persist. Initial and later passes can therefore perform
different amounts of I/O; preserve each pass and match its I/O counts when
comparing hardware with the emulator.

`report.py` requires every article/pass/viewport step, three repetitions,
positive ordered phase timings, actual 104-pixel movement, use of direct
scrolling, consistent article identities/sizes/pixels, healthy I/O and a
complete power-off run. Startup validation rejects missing phases, DMA
fallback and I/O errors. Its comparison function requires matching clock
configuration, output pixels and per-pass I/O work before computing phase
medians and individual pass errors. The caller must verify the app, boot,
archive and initial image identities before comparing reports.

## Current evidence

The final app SHA-256 is
`44362cfd278820af4ab07be1c2d0c7c64d18b0901722074333f870a75ab8668a`.
`build/wr128/workflow-calibration/ready-run/results.json` contains the verified
emulator run: 27 phase vectors, three samples each, nine article loads and
36 actual viewport movements with matching output checksums. It also retains
one startup report. That initial validation used the SDRAM/reset-fix binary
`551a4b3ec3d966931fa2e5d34984c16e51f6b9cba7ad81b22aa5f63d7bf24aa0`.
The later CPU data-access ordering and IDMA deadline binaries were replayed
with the identical fixture. The latest verified run is
`build/wr128/address-phase-review/final-workflow`; its 27 sample vectors
retain matching native outputs and per-pass I/O counts. The baseline binary
is `6d17536e23f87482869963ecc3d3b6d98e7109ea87515e29834b02a42ddde93d`.
The latest comparison is `build/wr128/address-phase-review/final-workflow-comparison.json`:
screens are +6.03--7.77%, forward steps +8.86--9.57%, and short return steps
about -14.4--14.8%. The older default comparison and statistics below refer
to the preceding IDMA baseline; neither is complete workflow qualification.

The physical report is now archived in `device.json`, with raw article and
startup reports under content hashes. Installed app/boot/list/launcher files,
both archives, seven fonts and the language resource were verified. All nine
article loads and 36 viewport steps match native pixels and per-pass I/O work.
The returned card was safely ejected.

```sh
python3 emulator/tools/workflow_bench/collect.py
python3 emulator/tools/workflow_bench/analyze.py
```

`analyze.py` verifies binary/input/report/resource hashes before comparing the
current production run. Its default output is
`build/wr128/workflow-calibration/hardware-comparison.json`. Of 27 phase
medians, 21 are within 10%, none within 5%; median absolute error is 7.89%.
Completed article screens take 6.08--7.89% longer in the model. Downward steps
take 9.18--9.95% longer; return steps take 14.28--14.73% less time (about
0.44 versus 0.51--0.52 ms). Retrieval outside card reads takes 6.58--6.86%
longer, while time inside reads differs by +0.46--3.48%. Read time remains a
subset of phase time.

The matched app-to-keyboard interval is 165.298 versus 156.227 ms (+5.81%).
Total kernel boot is excluded: launcher contents and file-read work differ.
Keep the one-cold-boot count and the physical-layout limitation explicit.

Raw card reads were unavailable. Relevant reader resources and starting files
were saved with ordinary file copies; the physical filesystem allocation was
not captured. Collection explicitly records `physical_layout_verified=false`.
The development fixture verifies the application but does not establish an
identical physical-card replay or timing qualification. No further raw-image
or administrator-access work is pending.

This advances lookup/render/scroll measurement. Full input-driven search,
coasting, repeated cold boots and suspend/wake qualification remain open,
as does the overall accuracy milestone in `emulator/ACCURACY.md`.

Use `run.py --profile` to retain opcode counts and per-instruction PC timing
in `profile.txt`, in addition to the usual executed input identities and raw
reports. The production-binary profile run is
`build/wr128/frontend-lab/reader-profile`: all article and startup report bytes
are identical to the unprofiled run. Whole-run profiles include untimed serial
output/checksums and startup, so they are not per-phase timing error budgets.
IVRAM overlay addresses have several residents; address-only profiles cannot
attribute those instructions to one overlay function. Preserve that region
as mixed residency unless the executed overlay identity is also captured.
