# Sparrow development handoff

Updated September 10, 2026. Implementation baseline: **`c75f2117`**
(`sparrow: add offline Wikidata answers and persistent query history`).
This is the starting point for the next development session. Read the live job
files before acting on the progress snapshot below.

## Current state

Sparrow works on the host and in the production C33 emulator. It is a bounded
offline Wikidata query engine integrated with the existing WikiReader UI;
full-corpus coverage and physical-device latency are still unmeasured.

- Shared C engine: factual lookups, up to three composed steps, age and
  historical office joins, distinct-item counts, positive relationship checks,
  parent comparison, grandchildren unions and exact-date year projection.
- Host builder: 57 selected properties, compressed SQLite staging, immutable
  CRC-protected device records, source statement audit and build manifest.
- UI: `ask ` search row, rendered evidence and local article links, Back,
  existing page cache and persistent 256-entry History. Evicted answers are
  regenerated from the installed index; History stores the question.
- Canonical English Wikipedia titles take precedence over unrelated aliases.
  Ambiguous canonical titles and unsupported claim semantics still abstain.
- The official full dump is downloaded and verified. Import and automatic
  benchmark evaluation are running as detached jobs; they do not install an
  index onto the demo or a physical SD card.

The working name is **Sparrow**. `wikibox-spec.md` retains the original proposal
and review; its early estimates are not measured performance promises.

## Constraints to preserve

- Target the owner's tested **32 MB, 60 MHz Epson C33** devices.
- Knowledge must come from **sanctioned Wikimedia dumps only**. No entity API,
  SPARQL endpoint or website scraping. Use the existing local Wikipedia ZIM
  for articles: `wikipedia_en_all_maxi_2026-02.zim`.
- Stream the compressed full dump. Never materialize the roughly 1.8 TB
  uncompressed JSON. Keep the 24 GiB free-space reserve and account for staging,
  audit and publication copies before starting another large build.
- Keep gold answers and gold-query execution out of knowledge ingestion.
  Develop rules on QALD training questions; freeze code/data before test runs.
  The QALD test set has already been evaluated, so do not present future runs
  as first exposure to an unseen benchmark or tune rules to its failures.
- Preserve ranks, qualifiers, units, date precision, calendars and statement
  IDs. Missing facts do not prove false or zero. Counts are explicitly of
  recorded distinct QIDs; incomplete evidence must not become a full answer.
- Preserve saved demo History. Use a separate emulator `--stage` for tests;
  `--fresh` intentionally discards the selected demo card's History.

## Running jobs: inspect these first

Observed **2026-09-10 12:44:44 EDT**: the full job was `importing`, with
**20,890,000 source records scanned**, about 15.4 GiB of staging
and 288.4 GiB free. These are a timestamped progress snapshot, not a
completion percentage or a full-corpus size estimate. The v3 evaluation was
waiting for the original job to finish.

| Job | Directory | Observed processes |
| --- | --- | --- |
| Full import and original evaluation | `build/sparrow/full-20260907/` | parent 37622, importer 37624 |
| Current engine and title-policy evaluation | `build/sparrow/full-20260907-v3/` | waiting worker 59109 |

PIDs are historical observations, not instructions to signal a process. Check
its command and current state if troubleshooting. A waiting evaluation worker
does not continuously refresh its `job.json` timestamp.

Run from the repository root:

```sh
cat build/sparrow/full-20260907/job.json
cat build/sparrow/full-20260907/build-status.json
tail -n 25 build/sparrow/full-20260907/run.log
cat build/sparrow/full-20260907-v3/job.json
tail -n 25 build/sparrow/full-20260907-v3/run.log
```

**Leave the current import and both `frozen/` bundles intact.** They contain
recorded binary, builder and dataset hashes. Starting another import is not
needed to collect this revision's results. The input is:

```text
build/wikidata/wikidata-20260907-all.json.bz2
103,048,420,178 bytes
SHA-1: 503b694182f1cbb98102dab60ba1fa0bbcb2a954
```

The adjacent `.status.json` records verification against the published hash.
The host environment is `build/sparrow/host-env/`, with `indexed_bzip2==1.7.0`
and `orjson==3.12.0`; the importer uses six decode workers.

The original job's frozen builder predates the latest title policy and flag
32 for unresolved item metadata. It still builds the 57-property index. After
publishing `sparrow.dat`, its manifest and `.statements.jsonl.gz` audit, it runs
`train-before.json`, `train-after.json`, `test-before.json`, `test-after.json`.

The queued v3 job waits for the original job to become `complete`. It creates
its own `sparrow.dat` with title precedence, preserving **every entity and
claim byte**, then runs these comparisons on both train and test splits:

| Report suffix | Engine | Index |
| --- | --- | --- |
| `before` | Original job's frozen current engine | Original full index |
| `operators` | Frozen current Sparrow engine | Original full index |
| `titles` | Frozen current Sparrow engine | Separate title-upgraded index |

Its audit remains at the source index path recorded in the new manifest.
This title upgrade cannot recover omitted metadata or change old missing-target
claim flags. Current readers accept the old conservative encoding; old readers
reject new flag-32 records. Install matching firmware with newly built data.
Do not clear an old unsafe flag blindly: it may also mean an unknown qualifier.

If a job fails, inspect its log and preserve the artifacts before choosing a
retry. Import is **not checkpoint-resumable**, and its staging database is
temporary. `--worker` is an internal entry point, not a resume command. If only
evaluation failed after publication, verify the index, manifest and audit and
rerun the evaluation rather than reimporting the dump. Use a new job directory
for a new build or evaluation; existing output is deliberately refused.

## What the benchmark actually establishes

[BENCHMARKS.md](BENCHMARKS.md) is the current measurement record; some README
examples describe earlier development samples.

| Measurement | Previous | Current |
| --- | --- | --- |
| Training questions whose wording is recognized | 98 / 371 | 108 / 371 |
| Test questions whose wording is recognized | 16 / 136 | 16 / 136 |
| Exact training answers, fixed 5.11-million-record prefix | 0 / 371 | 1 / 371 |
| Exact test answers, same prefix | 0 / 136 | 0 / 136 |
| Exact training answers, separate official geography demo | 5 / 371 | 5 / 371 |

The new exact training answer is Benjamin Franklin's child count, **3**.
There is no demonstrated test-score gain. Wording coverage is not answer
accuracy; the current grammar caps test coverage at 11.8% even with complete
data. Scores are strict normalized answer sets, not an official QALD score.
Six test questions have unscorable gold answer shapes and abstain.

The prefix is unordered and omits many common entities and referenced labels;
its size does not make it representative. Snapshot changes, units and calendar
differences also cause strict mismatches. Report those limitations rather than
changing answers or inventing exact dates to imitate gold values.

Current per-question results, baseline indexes, manifests, frozen inputs and
hashes are in `build/sparrow/coverage-v3/`; older reports remain in
`build/sparrow/coverage-v2/` and `build/sparrow/dump-eval/`.
Full-data scores are pending in the two job directories above.

## Next steps, in order

1. **Collect the full-data baseline.** Let both jobs finish. Check manifests
   and hashes, index/audit size, ambiguous aliases, unsafe claims, oversized
   property groups and omitted metadata. Summarize all three v3 comparisons in
   `BENCHMARKS.md`, keeping wording recognition, answered questions, exact
   matches, unscorable gold and snapshot mismatches separate.
2. **Choose the next operation from training failures on complete data.**
   Group failures by the existing `wording`, `entity`, `claims` and `bridge`
   diagnostics. Work from training questions and original dump statements;
   avoid more rule tuning around missing entities in a small prefix.
3. **Prototype a bounded reverse index if training results justify it.**
   The main remaining capability gap is finding subjects from an object,
   then filtering/intersecting candidates. A host-built `(property, object)`
   posting list could enable this without device-wide scans. Measure index
   size and candidate fan-out first, retain semantic qualifiers, and abstain
   on overflow. General filtering and ordering need explicit bounded operators;
   adding only paraphrases will not cover them.
4. **Improve semantics using independent cases.** Unsupported scope qualifiers,
   date precision/calendars and quantities need explicit representations and
   rules. Keep strict scoring for comparison; any new evaluator metric must
   be separately labeled. Add an adapter for another established dataset
   (RuBQ or LC-QuAD) and reserve an unused split for independent validation.
5. **Validate a full-size candidate on the device path.** First run the host
   engine against the final index, then a card/emulator setup that supports
   its actual size, and finally physical-device tests. Measure cold/warm
   latency, memory, SD activity and rendering; logical reads are not SD seeks.
   Exercise article links, History after reboot, eviction and corruption.
   Update this handoff with results and the next frozen revision.

Future host pipeline work should consider resumable staging and better failure
recovery after publication; neither is implemented. Do not restart the current
import to add those features. A new property or claim-encoding revision needs
a deliberate rebuild; the queued title-only upgrade cannot supply it.

## Run and verify the implementation

The current demo is `build/sparrow/sparrow.dat`, built from the official
September 1 multistream sample: 1,175 entities, 16,532 claims, 2,099,200 bytes.
Its `.json`, `.source.json` and `.statements.jsonl.gz` files retain build and
source provenance. The input JSONL is `build/sparrow/dump-sample.jsonl` with
`dump-sample.manifest.json`. This is a separate snapshot from the full JSON dump.

```sh
make -C sparrow check
make -C sparrow sanitize
make -C zim TOOLCHAIN_BIN="$PWD/host-tools/toolchain-c33/work/install/bin" SIMULATE=NO OPT=-O2
sparrow/build/sparrow build/sparrow/sparrow.dat 'how many children did george h. w. bush have'
python3 sparrow/emulator-check.py build/sparrow/sparrow.dat
```

The last command opens a live window, types the Berlin Wall question, opens
the answer and hands over control. Press **1** for Search and **2** for History.
Use `--query 'ask ...'` for another question. A separate automated run is:

```sh
python3 sparrow/emulator-check.py build/sparrow/sparrow.dat --headless \
  --stage build/sparrow/handoff-emulator \
  --query 'ask how many children did george h. w. bush have' --cycles 850000000
```

The recorded count screenshot is
`build/sparrow/coverage-v3/emulator-count/screen.png`. Its QID evidence is
expected: the sample lacks the children's display metadata. The count still
has six distinct source claims. Demo preparation retains existing History;
`--reuse-card` additionally requires the exact same firmware and data hashes.

**The current emulator helper builds a small FAT fixture in host memory.**
Do not assume it can accept a multi-gigabyte full index with bounded host RAM.
Full-size card preparation/emulation needs separate validation. The existing
macOS `zim/make-card-image` supports an exFAT index alongside the local archives.

Latest verification passed: 96 synthetic golden queries, 16 independent binary
comparisons, 110 malformed-file cases, set/fan-out/byte limits, title upgrade,
HTML conversion and persistent History tests; ASan/UBSan; C33 build; production
emulator replay. No physical SD installation or hardware latency measurement
was performed for this revision.

Relevant limits: three plan steps, eight returned values, nine evidence groups,
256 counted QIDs, 64 logical reads and 250 KiB per query. On C33 the context is
70,740 bytes and the static result 69,088 bytes, plus the existing file/page
caches. The UI accepts 63 input bytes including `ask `; the engine accepts 127.
See [FORMAT.md](FORMAT.md) for compatibility and [README.md](README.md) for usage.

## Code map and workspace housekeeping

| Area | Main files |
| --- | --- |
| Record reader, parser, bounded execution | `sparrow.c`, `sparrow.h`, `cli.c` |
| Host index and dump ingestion | `build.py`, `download.py`, `dump-sample.py`, `geography-seeds.py` |
| Build/evaluation orchestration | `full-build.py`, `evaluate-build.py`, `title-index.py`, `benchmark.py` |
| Preview export | `stage-preview.py` (macOS/APFS clone under a brief SQLite read lock) |
| Evidence pages | `html.c`, `html.h`, `../zim/zim_sparrow.c`, `../zim/search.c` |
| History and page integration | `../wiki/history.c`, `../wiki/lcd_buf_draw.c` |
| Live replay | `emulator-check.py`, `../emulator/tools/mem_dma_bench/run.py` |

Generated files are ignored by Git and are machine-local. The September 10
cleanup removed abandoned staging databases, the two completed preview SQLite
databases, retired API data, old 100k/250k prefixes and obsolete prototype/probe
outputs. Do not expect `coverage-v3/preview/snapshot.db` or
`coverage-v3/canonical-stage.db` to exist. Their published benchmark indexes,
audits and reports remain. The cleanup inventory is
`build/sparrow/cleanup-20260910.json`; official dumps, the active staging DB,
frozen jobs, host environment, benchmark repository, screenshots and demo cards
were preserved. Keep test source files: they are part of the implementation.

The baseline commit includes Sparrow's LCD initialization change in the
emulator loader harness. Separate, uncommitted UART work remains in
`emulator/Makefile`, `emulator/README.md`, `emulator/src/main.c`,
`emulator/src/uart.c`, `emulator/src/uart.h` and `emulator/tools/test_uart.c`.
Leave those edits out of Sparrow commits unless their scope is explicitly
expanded. Recheck Git status because another task may continue editing them.
