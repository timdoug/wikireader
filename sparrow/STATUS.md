# Sparrow status

Where the implementation stands and what comes next. See [DESIGN.md](DESIGN.md)
for the rationale, [FORMAT.md](FORMAT.md) for compatibility, [README.md](README.md)
for usage and [BENCHMARKS.md](BENCHMARKS.md) for the measurement record.

## What works

Sparrow runs on the host and in the production C33 emulator. It is a bounded
offline Wikidata query engine integrated with the existing WikiReader UI.
Full-corpus coverage and physical-device latency are unmeasured.

- **Shared C engine** - factual lookups, up to three composed steps, age and
  historical office joins, distinct-item counts, positive relationship checks,
  parent comparison, grandchildren unions, exact-date year projection.
- **Host builder** - 57 selected properties, compressed SQLite staging,
  immutable CRC-protected device records, source statement audit, build
  manifest.
- **UI** - `ask ` search row, rendered evidence, local article links, Back, the
  existing page cache and a persistent 256-entry History. Evicted answers are
  regenerated from the installed index; History stores the question.
- **Title resolution** - canonical English Wikipedia titles take precedence over
  unrelated aliases. Ambiguous canonical titles and unsupported claim semantics
  abstain.

## What the benchmark establishes

| Measurement | Previous | Current |
| --- | --- | --- |
| Training questions whose wording is recognized | 98 / 371 | 108 / 371 |
| Test questions whose wording is recognized | 16 / 136 | 16 / 136 |
| Exact training answers, fixed 5.11-million-record prefix | 0 / 371 | 1 / 371 |
| Exact test answers, same prefix | 0 / 136 | 0 / 136 |
| Exact training answers, separate official geography demo | 5 / 371 | 5 / 371 |

The one exact training answer is Benjamin Franklin's child count, **3**. There
is no demonstrated test-score gain. Wording coverage is not answer accuracy: the
current grammar caps test coverage at 11.8% even with complete data. Scores are
strict normalized answer sets, not an official QALD score, and six test
questions have unscorable gold answer shapes and abstain.

The record prefix is unordered and omits many common entities and referenced
labels, so its size does not make it representative. Snapshot changes, units and
calendar differences also cause strict mismatches. These are limitations to
report, not to paper over by loosening answers.

**Full-data scores have not been measured.** The complete dump is downloaded and
verified against its published SHA-1 (103,048,420,178 compressed bytes); a full
index has not been published from it.

## Next steps, in order

1. **Run the full-data baseline.** Check manifests and hashes, index/audit size,
   ambiguous aliases, unsafe claims, oversized property groups and omitted
   metadata. Summarize frozen before/after results in `BENCHMARKS.md`, keeping
   wording recognition, answered questions, exact matches, unscorable gold and
   snapshot mismatches separate.
2. **Choose the next operation from training failures on complete data.** Group
   failures by the existing `wording`, `entity`, `claims` and `bridge`
   diagnostics, working from training questions and original dump statements.
   Avoid further rule tuning around entities merely missing from a small prefix.
3. **Prototype a bounded reverse index if training results justify it.** Measure
   index size and candidate fan-out first, retain semantic qualifiers, and
   abstain on overflow. General filtering and ordering need explicit bounded
   operators; adding paraphrases alone will not cover them.
4. **Improve semantics using independent cases.** Unsupported scope qualifiers,
   date precision/calendars and quantities need explicit representations and
   rules. Keep strict scoring for comparison and label any new evaluator metric
   separately. Add an adapter for RuBQ or LC-QuAD and reserve an unused split
   for independent validation.
5. **Validate a full-size candidate on the device path.** Host engine against
   the final index first, then a card/emulator setup that supports its actual
   size, then physical-device tests. Measure cold/warm latency, memory, SD
   activity and rendering. Exercise article links, History after reboot,
   eviction and corruption.

The host pipeline has no resumable staging and no failure recovery after
publication; both are worth building before the next large job. Import is not
checkpoint-resumable, so a failed run restarts the dump scan. If only evaluation
fails after a successful publication, verify the index, manifest and audit and
rerun evaluation rather than reimporting.

## Running a full build

From the repository root:

```sh
make -C sparrow
build/sparrow/host-env/bin/python sparrow/full-build.py \
  build/sparrow/full-<date> \
  --baseline build/sparrow/coverage-v3/frozen/sparrow --start
```

A fresh output directory is required; existing output is deliberately refused.
The job runs a full scan with six decode workers and a 24 GiB free-space
reserve, then train/test evaluation. It produces `sparrow.dat`, its `.json`
manifest and `.statements.jsonl.gz` audit, then `train-before.json`,
`train-after.json`, `test-before.json` and `test-after.json`. It does not
replace the installed demo or write a physical SD card.

Use the v3 baseline: it understands the current claim flags, where the older
default baseline predates flag 32. If the engine has not changed, before/after
scores will be identical - the absolute full-data results are still the point.
The current builder already includes canonical-title precedence and flag 32, so
a fresh build needs no title-upgrade follow-up.

Progress is in `job.json`, `build-status.json` and `run.log` in the job
directory.

## Compatibility

Current readers accept the old conservative encoding; old readers reject new
flag-32 records, so install matching firmware with newly built data. An old
unsafe flag may also mean an unknown qualifier, so do not clear it blindly. The
`title-index.py` helper only changes aliases - it cannot recover omitted
metadata or change old missing-target claim flags, and a new property or
claim-encoding revision needs a deliberate rebuild.

## Run and verify

The demo index is `build/sparrow/sparrow.dat`, built from the official
September 1 multistream sample: 1,175 entities, 16,532 claims, 2,099,200 bytes.
Its `.json`, `.source.json` and `.statements.jsonl.gz` files carry build and
source provenance. The input JSONL is `build/sparrow/dump-sample.jsonl` with
`dump-sample.manifest.json` - a separate snapshot from the full JSON dump.

```sh
make -C sparrow check
make -C sparrow sanitize
make -C zim TOOLCHAIN_BIN="$PWD/host-tools/toolchain-c33/work/install/bin" SIMULATE=NO OPT=-O2
sparrow/build/sparrow build/sparrow/sparrow.dat 'how many children did george h. w. bush have'
python3 sparrow/emulator-check.py build/sparrow/sparrow.dat
```

The last command opens a live window, types the Berlin Wall question, opens the
answer and hands over control. Press **1** for Search and **2** for History. Use
`--query 'ask ...'` for another question. Headless:

```sh
python3 sparrow/emulator-check.py build/sparrow/sparrow.dat --headless \
  --stage build/sparrow/status-emulator \
  --query 'ask how many children did george h. w. bush have' --cycles 850000000
```

Test against a separate `--stage`; `--fresh` intentionally discards the selected
demo card's History, and demo preparation otherwise retains it. `--reuse-card`
additionally requires identical firmware and data hashes.

**The emulator helper builds a small FAT fixture in host memory.** It cannot be
assumed to accept a multi-gigabyte full index with bounded host RAM; full-size
card preparation and emulation need separate validation. The macOS
`zim/make-card-image` supports an exFAT index alongside the local archives.

Latest verification: 96 synthetic golden queries, 16 independent binary
comparisons, 110 malformed-file cases, set/fan-out/byte limits, title upgrade,
HTML conversion and persistent History tests; ASan/UBSan; C33 build; production
emulator replay. No physical SD installation or hardware latency measurement has
been performed.

## Limits

Three plan steps, eight returned values, nine evidence groups, 256 counted QIDs,
64 logical reads and 250 KiB per query. On C33 the context is 70,740 bytes and
the static result 69,088 bytes, on top of the existing file and page caches. The
UI accepts 63 input bytes including `ask `; the engine accepts 127.

## Code map

| Area | Main files |
| --- | --- |
| Record reader, parser, bounded execution | `sparrow.c`, `sparrow.h`, `cli.c` |
| Host index and dump ingestion | `build.py`, `download.py`, `dump-sample.py`, `geography-seeds.py` |
| Build/evaluation orchestration | `full-build.py`, `evaluate-build.py`, `title-index.py`, `benchmark.py` |
| Preview export | `stage-preview.py` (macOS/APFS clone under a brief SQLite read lock) |
| Evidence pages | `html.c`, `html.h`, `../zim/zim_sparrow.c`, `../zim/search.c` |
| History and page integration | `../wiki/history.c`, `../wiki/lcd_buf_draw.c` |
| Live replay | `emulator-check.py`, `../emulator/tools/mem_dma_bench/run.py` |

Build outputs under `build/` are Git-ignored and machine-local. Test source
files are part of the implementation, not scratch.
