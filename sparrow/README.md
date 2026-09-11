# Sparrow

A small offline factual answer engine for our **32 MB, 60 MHz WikiReaders**.
It uses Wikidata claims and opens articles in the existing local ZIM reader.
The device executes ordinary C lookup code, without model weights or a network.

This is a working first implementation, not yet a full-Wikidata release.
The host and C33 builds share the query engine. Knowledge data comes only from
published Wikimedia dump files. The small demo is explicitly a sample, extracted
from a dated multistream dump. The full dated JSON dump is downloaded compressed
and streamed during import. No uncompressed full dump file is needed. There is
no API or SPARQL fetching workflow.

For continuing development, start with [STATUS.md](STATUS.md): what works,
current results and ordered next steps. [DESIGN.md](DESIGN.md) explains why the
engine is built this way, and [BENCHMARKS.md](BENCHMARKS.md) has the latest
measured coverage.

## Use

Build the host tools and run the regression suite:

```sh
make -C sparrow check
make -C sparrow sanitize
```

Build an index from a Wikidata JSON dump (`.json`, `.jsonl`, `.gz`, or `.bz2`):

```sh
python3 sparrow/build.py build/wikidata/wikidata-20260907-all.json.bz2 \
  build/sparrow/full/sparrow.dat --snapshot wikidata-20260907 \
  --scratch build/sparrow --reserve-gib 24
sparrow/build/sparrow build/sparrow/full/sparrow.dat 'capital of burkina faso'
```

The builder uses SQLite **on the host** for disk-backed staging and sorting.
Original statement JSON is compressed inside those staging tables to reduce
scratch space. This changes neither the original-claim audit nor the device
format. Optional pinned host dependencies in `requirements-host.txt` provide
[parallel bzip2 decoding](https://github.com/mxmlnkn/librapidarchive/tree/master/python/indexed_bzip2)
and faster JSON parsing; the standard-library path remains supported.
It reads the dump once, keeping metadata and selected statements. Related items
without English Wikipedia pages remain eligible, including units and offices.
English and shared `mul` labels are supported. Device files contain neither
SQLite nor JSON. A compressed statement audit preserves original qualifiers,
references and statement IDs, and a JSON manifest records counts and SHA-256.
Output files are published after a successful build; existing files are refused.

Full-corpus import throughput, peak scratch use, and coverage are not yet
measured. The staging guard checks free space every 10,000 entities and before
publication, with a default 24 GiB reserve. Run on a volume with enough scratch;
the reserve is a stop condition, not a promise that any corpus will fit. The
builder bounds oversized entities by replacing whole property groups with an
unsafe marker, making those properties abstain while retaining their original
statements in the host audit. Oversized labels display their QID; oversized
article titles lose their link. The manifest counts these cases. Their frequency
must be measured on the full dump before releasing an index.

Build the firmware normally:

```sh
make -C zim
```

Install `zim/zim.app` and put `sparrow.dat` in the exFAT content volume root (or
the FAT32 boot volume root). In the existing search keyboard type
`ask capital of burkina faso`, then tap **Tap to answer this question**.
An answer page shows the recorded values, each lookup step, date qualifiers,
the dataset snapshot, and Wikidata statement IDs. Tap underlined names to open
their article in the currently selected archive. Links resolve by the exact
English Wikipedia title at tap time, so an index cannot send a stale ZIM entry
number into a different archive. Missing articles do not open.

Ordinary title search continues to work. Ask does not load its data until an
answer is requested. A missing or invalid index produces a visible error page.
For a new card image, `zim/make-card-image OUTPUT.dmg --sparrow FILE ZIM_FILE`
installs the index alongside the archives.

Question pages share the existing **256-entry History**, Back navigation, scroll
positions, save scheduling and four-page / 2.5 MiB article cache. History stores
`ask <complete question>` instead of the answer heading. Opening an evicted answer
reruns the saved question; after a reboot every answer is regenerated from the
installed index. Runtime IDs are rebuilt on startup, so an old ID cannot become
a different question. Duplicate visits move the question to the top. Clearing
History clears saved questions too. The transient store retains 32 recent
questions for Back navigation. New IDs are bounded to 4,095 per boot.

History read errors, malformed records and a full 256-entry file are bounded;
failed or short writes leave History dirty for a later retry. Persistence has
the same best-effort durability as ordinary pages. The keyboard limits complete
input to 63 bytes (including `ask `); the standalone engine accepts 127 bytes.

## Supported questions and abstention

Examples include `capital of X`, `X birthplace`, `when was X born`,
`X spouse`, `X official language`, `X currency`, `X population`, `X borders`,
`how tall is X`, `how old was X at death`,
`what language do they speak where X is`, and
`who was OFFICE when EVENT`.

Additional patterns cover authors, directors, composers, developers, creators,
founders, parents, place/cause of death, education and doctoral supervisors.
Two-hop examples include `where did the architect of X study`. These patterns
were developed from the QALD training split; the held-out questions are not a
source of aliases, facts or special-case answers.

The builder currently selects 57 properties. Further patterns cover ownership,
mayors, burial, residence, awards, instruments, programming languages, time
zones, employees, area, elevation, duration and English birth names. Explicit
relation phrases compose up to three steps, for example `population of the
capital of the country of X`. Every intermediate result must still be one
unqualified item. Negation and unsupported modifiers remain significant.

Additional operators count distinct recorded children, official languages,
borders and awards; prove positive relationships; compare recorded parents;
and follow multiple family branches. Examples:

```text
How many children did Benjamin Franklin have?
Did X have children?
Was X married to Y?
Did X study at Y?
Do X and Y have the same parents?
Give me the grandchildren of X.
In which year was X born?
```

Counts use at most 256 distinct QIDs, deduplicate repeated claims and say
"recorded" because absence from Wikidata does not establish a complete census.
Missing display metadata does not prevent a count. Unknown scope qualifiers
still make it abstain. An absent property is not interpreted as zero or false.
Relationship checks answer Yes only with a supporting claim. Comparing parents
requires exactly one unqualified father and mother for each person. The special
grandchildren operator visits up to eight children and returns up to eight
distinct grandchildren; other composed queries still need one bridge value.
Year extraction currently requires an exact Gregorian source date.

`sparrow/build/sparrow --plan 'population of the capital of Australia'` shows
the same plan the device executes without opening an index. JSON query output
and benchmark reports distinguish wording, entity lookup, claims and bridge
failures. A recognized plan alone does not establish answer coverage.

Templates match the **entire** question before any lookup. Alias verification
compares the complete normalized string; unknown words cannot pass a short
fingerprint check. Normalization folds ASCII case, underscores and whitespace;
it preserves UTF-8, accents, negation and punctuation. Typo tolerance exists
only for aliases actually in the data or an explicit `--aliases` JSON map.
A unique normalized English Wikipedia article title takes precedence over
colliding aliases. Multiple normalized article titles remain ambiguous, as do
other alias collisions; candidates are not ranked by popularity.

Facts preserve units, ranks, start/end/as-of dates and original source IDs.
Deprecated statements are excluded. Ordinary queries select the highest
nondeprecated rank and deduplicate identical surviving facts, with at most eight answers.
Unhandled scope qualifiers, unknown values, missing targets and over-limit
answers abstain. This is intentionally conservative: the actual Eiffel Tower
height claims contain part/scope qualifiers which this version does not yet
interpret, so it abstains instead of flattening them to an unqualified 330 m.
`partner`, `bigger`, context-free succession, and superlatives are not inferred.
Gregorian year/month qualifiers are displayed at their actual precision, such
as `as of 2020` or `through 2020-05`. They never become invented exact days.

Temporal office joins invert P39 statements onto the office entity. They use
all nondeprecated historical terms, with Gregorian day precision, and require
unambiguous coverage of the event day. Unknown dates that might overlap and
transition days with multiple holders abstain. The Berlin Wall demo uses
**fall of the Berlin Wall (Q69163529), P585**, not the wall's demolition date.
Age uses exact birthdays, not elapsed days divided by an average year.

Two-hop traversal currently requires exactly one unqualified bridge value.
No broad closure, embeddings, extractive fallback, or Markov mode is included.
The fallback is the reader's existing title search. This checkout does not
implement Xapian full-text search.

## Storage and resource bounds

[FORMAT.md](FORMAT.md) describes the provisional little-endian format.
Alias hash buckets, entity blocks and directories are addressed with 64-bit
file offsets. Exact alias strings and answer labels are stored in the blocks
needed by the query. CRC-32 protects the header and each block; the distribution
manifest carries a full-file SHA-256. Corrupt or incompatible data fails closed.

The C query context has a 64 KiB entity buffer, a 4 KiB alias buffer and a 1 KiB
set buffer. Plans have at most three steps; branched family queries may retain
nine evidence groups of eight claims. On C33 the context is 70,740 bytes and
the static result is 69,088 bytes. The firmware also reuses the existing
128 KiB `ZIM_FILE` cache. There is no multi-megabyte mandatory index at startup.
The current engine enforces 64 logical reads and 250 KiB per query; the supported
ordinary regression cases take at most twelve logical reads, and an eight-branch
family fixture takes twenty. Separate tests exercise the set and byte limits.
Logical reads include small
on-card directory lookups and are **not measured SD commands or seeks**.

Read/byte counts depend on the built index and are printed by the CLI. They
establish an access pattern, not a hardware latency claim. Target timing must
include cache misses, filesystem mapping, CRC, CPU work and rendering.

## Reuse and independent evaluation

[BENCHMARKS.md](BENCHMARKS.md) records the latest comparisons and limitations.
The current grammar recognizes 108/371 training and 16/136 test questions;
full-data answer scores are pending. The older small-sample results below
remain as development history.

The existing FatFs/64-bit file adapter, ZIM parser, article rendering, keyboard,
links, fonts, emulator, and FAT test-image builder are reused directly.
Python's JSON, bz2, gzip, SQLite and decimal libraries handle the host build.
The new embedded component is a bounded record reader and template executor.

[qEndpoint](https://github.com/the-qa-company/qEndpoint) and
[HDT C++](https://github.com/rdfhdt/hdt-cpp) are relevant upstream designs:
dictionary IDs, immutable indexed graphs and constrained triple-pattern access.
They remain candidates for a host-side extraction backend. Their Java/C++
server/library implementations are not linked into the C33 firmware. We have
not established that nobody has built a similar embedded QA system.

[QALD-9-plus](https://github.com/KGQA/QALD_9_plus) provides 371 Wikidata training
questions and 136 test questions. Download it independently, retain its license,
and run untouched English questions against the binary engine:

```sh
git clone --depth 1 https://github.com/KGQA/QALD_9_plus.git build/sparrow/qald-9-plus
python3 sparrow/benchmark.py \
  build/sparrow/qald-9-plus/data/qald_9_plus_test_wikidata.json \
  build/sparrow/full/sparrow.dat --output build/sparrow/qald-test.json
```

The runner uses stored gold answers; it never executes the gold SPARQL, adds
facts from the benchmark, or changes the question sent to the engine. It reports
answer coverage, exact answer sets, selective accuracy, unscorable gold, and
per-question outcomes. This is **not an official GERBIL/QALD score**, and data
snapshot drift must be considered. Property counts are reported separately;
triple frequency in Freebase does not imply question coverage in Wikidata.
Small and partial dump samples do not establish full-corpus coverage. In
particular, an early JSON dump prefix is not ordered by QID and can omit very
common entities; apparent alias uniqueness in such a sample is provisional.

The September 1 dump sample contains 211 roots selected from the original demo
and the local Wikipedia sovereign-state list, plus 964 referenced entities.
Its 1,783,808-byte index has 12,887 selected claims and 5,714 aliases. Before/after
runs use this same source JSONL, the same 34 properties and the same aliases;
the baseline keeps the earlier templates and qualifier handling. Empty-map
compatibility for XML revisions was applied to both builders. New rules were
developed on training questions and frozen before running the held-out split.

| QALD-9-plus split | Before: answered / exact | After: answered / exact |
| --- | --- | --- |
| Training (371) | 2 / 2 | 4 / 4 |
| Held-out test (136) | 0 / 0 | 0 / 0 |

The additional training answers are Suriname's official language and the Czech
Republic's currency. All four returned training answers match the stored gold
exactly; 367 questions abstain. This is a small training improvement, with no
held-out coverage gain established. Six test questions have gold answer shapes
the strict evaluator cannot score; they also abstain. Per-question reports,
source hashes and baseline identities are under `build/sparrow/dump-eval/`.

[RuBQ](https://github.com/vladislavneon/RuBQ) and
[LC-QuAD 2.0](https://github.com/AskNowQA/LC-QuAD2.0) are further sources for
future adapters, especially paraphrases and multiple-triple queries. They have
not yet been imported or evaluated. The synthetic regression fixture remains
separate: it tests corruption, ambiguity, qualifiers, boundaries and exact dates
that a natural-language benchmark cannot replace.

## Official downloads and a small dump sample

```sh
python3 sparrow/download.py build/wikidata --start
```

The full download is pinned to September 7, 2026: 103,048,420,178 compressed
bytes. It resumes a `.part` file, verifies the published SHA-1, and renames the
file only after verification. It never decompresses the dump and reserves
24 GiB free. The detached worker writes a status JSON and download log beside
the file. It does not automatically launch a full import. Stop the worker with
its recorded PID if needed; its partial download remains resumable.

Once that download is verified, build and evaluate in the background with:

```sh
python3 -m venv build/sparrow/host-env
build/sparrow/host-env/bin/pip install -r sparrow/requirements-host.txt
build/sparrow/host-env/bin/python sparrow/full-build.py \
  build/sparrow/full-20260907-rerun \
  --baseline build/sparrow/coverage-v3/frozen/sparrow --start
```

Use a new output directory for each job; existing work is refused. The job
freezes the builder, binaries and benchmark inputs, reads the local dump with
six decode workers, and runs the train/test comparisons after a successful
build. `job.json`, `build-status.json` and `run.log` show progress and errors.
It keeps the full index separately for review. No network is used by this job.
Full-data results have not been measured yet; see [STATUS.md](STATUS.md).
The explicit v3 baseline above supports the current claim flags; the older
default baseline does not understand flag 32. A fresh build already includes
canonical-title precedence, so it does not need a title-upgrade follow-up.

To evaluate a newer frozen engine after an import already in progress:

```sh
python3 sparrow/evaluate-build.py build/sparrow/full-20260907-rerun \
  build/sparrow/full-20260907-rerun-next --start
```

This optional helper is not currently queued. It preserves the source job,
waits for completion, creates a separate index with canonical-title precedence,
and compares the previous and current engines on the original index before
evaluating the title change. Claim bytes and original missing-target handling
are preserved. The new directory contains its own frozen inputs, `job.json`,
`run.log`, index manifest and six benchmark reports. It budgets for a copy of
the index plus a 24 GiB reserve. The statement audit stays with the source index.

On macOS/APFS, `stage-preview.py JOB OUTPUT` exports an independent partial
index during import using a brief SQLite read lock and a filesystem clone.
It never performs a slow file copy while holding that lock. Such a snapshot
contains an unordered prefix, not a representative sample of Wikidata.

The small dump sampler reads the published September 1 multistream XML dump
using its published index. `dump-sample.py` verifies the complete compressed
index against the official `dumpstatus.json`, requests exact compressed ranges
from the matching dated dump file, checks Content-Range/length and bzip2 CRCs,
and records range hashes and revision IDs. It does not claim to verify the
whole XML dump's checksum when downloading only ranges. Original statements
come from the XML's entity JSON, with no API captures mixed in.

`geography-seeds.py` can derive additional sample roots from the sovereign-state
list and authority-control IDs in the **local** English Wikipedia ZIM. Its
selection is independent of the question benchmarks. The sample retains full
claims for its roots and only metadata for directly referenced leaves; its
manifest records missing entities and this deliberate incompleteness.

```sh
python3 sparrow/geography-seeds.py wikipedia_en_all_maxi_2026-02.zim \
  build/sparrow/geography-seeds.json
python3 sparrow/dump-sample.py build/sparrow/dump-sample.jsonl \
  --index build/wikidata/wikidatawiki-20260901-pages-articles-multistream-index.txt.bz2.part \
  --status build/wikidata/20260901-dumpstatus.json \
  --seeds build/sparrow/geography-seeds.json
python3 sparrow/build.py build/sparrow/dump-sample.jsonl build/sparrow/dump-demo/sparrow.dat \
  --snapshot dump-sample-20260901 --aliases sparrow/demo-aliases.json
```

The official index and manifest are available from
[Wikimedia's dated dump directory](https://dumps.wikimedia.org/wikidatawiki/20260901/).
The former API fetcher and retired API caches have been removed.
The demo aliases map names to entities, never questions to answers.

The emulator demo opens a live window, powers it on, types the Berlin Wall
question on the touchscreen keyboard, and taps the answer. It then leaves the
window open for you: click underlined links, drag to scroll, or press **1** for
Search and try another question. Press **2** for History. Close the window or press **Esc** to finish;
Ctrl-C in the terminal also stops the emulator cleanly.

```sh
python3 sparrow/emulator-check.py build/sparrow/sparrow.dat
```

For an automated screenshot run without a window, add `--headless`. Progress
appears immediately and during the replay; detailed output goes to
`build/sparrow/emulator/run.txt`. On exit the script saves `screen.pgm`, plus
`screen.png` when macOS's `sips` converter is available. `--query 'ask ...'`
changes the typed question; `--tap-link` also opens the linked Bush article.
Only one demo session can use the disposable card at a time.
Rebuilding a demo card preserves its existing History; `--fresh` explicitly
discards it. `--reuse-card` boots the exact existing card without rebuilding,
and `--history` reopens its first saved entry. `--stage PATH` isolates a test
from an interactive session that is still open.

The demo uses article HTML extracted from the local full English ZIM and the
production SD file-loader, kernel and app in a disposable FAT image. It does
not exercise the serial-FLASH boot menu. The loader harness supplies the
inherited stack and enabled LCD state that the omitted boot stages leave;
the firmware and the normal emulator's reset behavior remain unchanged.

Extracted article text retains its Wikipedia licensing footer. The generated
card and screenshots stay under `build/sparrow/emulator/`; no physical SD is written.

The first 100,000 records of the incoming compressed dump have also completed
a streaming import: 83,981 included entities, 183,053 normalized aliases,
297,195 selected claims, and 62,640,128 artifact bytes. This is an explicitly
partial dataset and does not establish full-dump resource use or coverage.

On that partial import, the untouched QALD-9-plus test set produced two
answers and 134 abstentions. Both answers failed strict literal equality:
Jack Wolfskin's founding date is stored with year precision (1981), while
the benchmark gives January 1; Muhammad's death is stored as June 8, 632 in
the Julian calendar, while the benchmark gives June 11. The reader retains
those distinctions. This is not evidence of a 0% factual-accuracy rate, nor
is it a useful full-corpus coverage score. It demonstrates why a future
benchmark adapter needs precision/calendar-aware comparison without silently
inventing exact days for year-only statements. The current report remains
strict and does not award these cases a match.

Firmware validation also exercised the 32 MB emulator's keyboard, question
page, and local article links. The dump-only demo's full question was verified
in `zim.hst`, then reopened from History after a fresh emulator boot. Host
checks include 63 golden queries, 14 independent binary fact comparisons,
110 malformed-file cases, sparse file offsets above 4 GiB, AddressSanitizer/
UndefinedBehaviorSanitizer, answer HTML passed through the production converter,
and the production history code exercised against restart, cache eviction,
scroll positions, corruption, full lists, failed writes and clear operations.
The existing ZIM host regression suite passes. The physical SD card
has not been changed; physical-device latency remains to be measured.
