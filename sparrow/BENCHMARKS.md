# Coverage measurements - September 10, 2026

The current frozen grammar recognizes **108 / 371 training questions** and
**16 / 136 test questions** in QALD-9-plus. The previous revision recognized
98 and 16. Recognition means the wording produces a plan; it does not mean
the entities resolve, the facts are present, or the resulting answer is right.

The test coverage ceiling remains **11.8%** even with complete data. Reverse
lookups, general filtering and ordering still need new query operations and
indexes. The design's earlier 25-40% figure remains an unverified aspiration.

## Current revision: counts, relationships and title resolution

New operations count distinct recorded items, prove positive relationships,
compare parents and union multiple family branches. Year projection requires
an exact Gregorian date. Counts can use original numeric QIDs even when a
partial index lacks their display metadata. Unsupported qualifiers still make
them abstain, and absence is never converted to a zero count or a false fact.

A unique normalized English Wikipedia title now takes precedence over
colliding Wikidata aliases. Multiple canonical titles that normalize to the
same key remain ambiguous. No popularity ranking or benchmark QID list is used.

Rules were developed on the training split, then frozen before this revision's
test evaluation. Knowledge comes exclusively from published Wikimedia dumps.
Gold queries are never executed and gold answers never enter the index. The
metric is strict normalized answer sets, **not an official QALD score**.

| Dataset and split | Previous: answered / exact | Current: answered / exact |
| --- | --- | --- |
| Official geography/demo sample, train (371) | 5 / 5 | 5 / 5 |
| Consistent 5.11-million-record prefix, train (371) | 1 / 0 | 3 / 1 |
| Same prefix, test (136) | 1 / 0 | 2 / 0 |

There is **one new exact training answer and no demonstrated test-score gain**.
"How many children did Benjamin Franklin have?" returns **3**, supported by
three distinct P40 QIDs in the dump. It takes four logical reads and 5,140 bytes
on this index. The demo sample also still abstains on all 136 test questions.

Both sides of the prefix comparison use the same consistent SQLite snapshot
of the active import: 5,110,000 source records, 1,040,681 included entities,
1,585,000 aliases and 3,059,604 stored claim records. Both indexes are
755,427,840 bytes. No additional source claims were fetched for the new score.
The new encoding distinguishes unresolved object metadata from unsafe claims;
the alias policy resolves 12,252 previously ambiguous keys using article titles.
20,535 keys remain ambiguous.

To separate the effects, the new engine was also evaluated on the original
prefix index: its results remain 1 answered / 0 exact on each split. The
Franklin improvement needs both the count operator and the encoding/title
changes. Older binaries cannot read the new unresolved-object flag, so the
overall before/after comparison necessarily includes an index rebuild.

The prefix is not a representative corpus: Wikidata's JSON dump is not ordered
by QID. This snapshot still lacks many common subjects and referenced items,
including the intended Canada, United Kingdom, Obama and Einstein entities.
An apparently unique alias in a prefix can become ambiguous on the full dump.

Strict failures include Rotterdam's mayor differing from the stored historical
answer, Michael Jordan's raw height quantity differing from the stored number,
Jack Wolfskin's founding year lacking the exact day expected by the benchmark,
and Muhammad's recorded Julian date differing from its stored answer. The
evaluator does not convert quantity units or Julian dates. These remain scored
as failures; no dates or values were changed to imitate the gold answers.
Six test questions have unscorable gold answer shapes; all six abstain.

Frozen sources, binary and dataset hashes, index manifests and per-question
results are under `build/sparrow/coverage-v3/`. `frozen.json` records the freeze;
`train-before.json`, `train-canonical.json`, `test-before.json`,
`test-operators.json` and `test-canonical.json` contain the prefix comparisons.
`demo-*.json` reports the separate small sample. Prior revision reports remain
under `build/sparrow/coverage-v2/` and `build/sparrow/dump-eval/`.

## Full-data evaluation

The complete September 7 Wikidata JSON dump is downloaded and verified against
its published SHA-1. It is 103,048,420,178 compressed bytes. The active local
import streams it through parallel bzip2 decoding into compressed SQLite
staging; it never writes the uncompressed full dump. A 24 GiB free-space guard
applies during import, emission and publication.

The original job, `build/sparrow/full-20260907/`, retains its frozen builder and
binaries and runs its original before/after evaluation when the index finishes.
`job.json` reports the overall state; `build-status.json` records source records,
scratch use and index progress. That job has not been restarted for this revision.

An additional job is queued at `build/sparrow/full-20260907-v3/`. After the
original job completes, it creates a separate copy with canonical-title
precedence, leaving every entity and claim byte unchanged. In particular,
missing-target handling stays conservative because the original builder used
the old encoding. It freezes the current engine and evaluates three arms on
both splits: previous engine/original index (`*-before.json`), current
engine/original index (`*-operators.json`), and current engine/title-upgraded
index (`*-titles.json`). Its `job.json` and `run.log` show progress.

**Full-data answer scores are pending.** These jobs use only local verified
dump data and stored benchmark files; no API or SPARQL requests are involved.

## Device validation

The suite passes 96 golden synthetic queries, 16 independent decoding
comparisons and 110 malformed-file cases, plus explicit 256/257-item count,
eight-branch family, byte-budget and title-upgrade checks. Address/undefined
behavior sanitizers, persistent History tests and the C33 firmware build pass.
Synthetic fixtures test the implementation; they do not count toward QALD.

The production C33 emulator types `ask how many children did george h. w. bush
have` and renders **6 recorded children** from the official demo dump sample.
Its screenshot is `build/sparrow/coverage-v3/emulator-count/screen.png`. The
sample lacks those children's metadata, so the evidence displays their QIDs.
The host query reads 8,523 bytes in four logical reads. No physical-device
latency measurement is claimed.
