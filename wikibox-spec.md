# Sparrow - review and implementation direction

The original proposal below is retained as design history. **Sparrow** is the
working name of the implementation in [`sparrow/`](sparrow/README.md). These review
findings supersede conflicting assumptions in the proposal.

- **The core approach is sound for our tested 32 MB units.** The 32 MB target
  is confirmed by the owner. The existing reader already consumes substantial
  RAM; a separate 19 MB allocation budget must account for that. The first
  implementation adds bounded buffers and reuses the existing file cache.
- **Start with runtime joins and measure.** The same portable C executes on
  the host and C33, so runtime joins can be tested off-device just as thoroughly
  as materialized ones. A 15 GB closure needs measured benefit before adoption.
  The dump-sample Berlin Wall join uses eight logical reads / 4,053 bytes;
  no subsecond hardware latency or 25-40% coverage claim has been established.
- **Retain Wikidata semantics.** Eight-byte triples cannot represent ranks,
  unknown values, units, precision, scope and qualifier dates. Excluding every
  entity without an enwiki sitelink also loses units, offices and bridge nodes.
  The implementation includes referenced entities, original statement IDs and
  an audit of qualifiers/references. It abstains on unsupported qualification.
  See the [Wikibase JSON model](https://doc.wikimedia.org/Wikibase/master/php/docs_topics_json.html).
- **A lookup can still give a wrong answer.** A 16-bit fingerprint admits false
  positives; aliases collide; source claims can be incomplete or inconsistent.
  Exact alias verification, CRC-protected blocks and explicit ambiguity are
  required. The promise is traceable recorded claims, not infallibility.
- **Several demos need semantic corrections.** Use the fall-of-the-Wall event
  Q69163529/P585, not demolition of the wall. Succession needs an office/term;
  Churchill had multiple terms. "Bigger" lacks a measure, date and geographic
  scope. Age requires birthdays and calendar precision. Population and height
  qualifiers cannot be discarded to make the demo pass. Print source dates.
- **The storage backend is real; Xapian search is not implemented here.**
  `zim/search.c` performs title-prefix search. Reuse local ZIMs for article
  reading and links; don't assume an embedded full-text fallback already exists.
- **Correct the hardware arithmetic.** This is Epson C33, not ARM: `UMULL` is
  not its instruction. C33 provides multiply and step-division instructions;
  avoiding unnecessary division helps, but banning all division is unnecessary.
  Likewise, the binary-embedding proposal omits an on-device query encoder.
- **Sizes and timings in the proposal are estimates, some contradictory.**
  A 19.4 GB index cannot also fit an 8 GB card; dropping 950 MB does not remove
  the 15 GB closure. A 4 KiB transfer is not free compared with 512 bytes at
  this bandwidth. Measure actual SD reads, transfer bytes and CPU/render time.
- **Use existing QA data for coverage.** Keep synthetic regressions for binary
  corruption and semantic boundaries, and use untouched held-out questions
  from [QALD-9-plus](https://github.com/KGQA/QALD_9_plus) for independent
  evaluation. It has 371 Wikidata train / 136 test questions, not 2,000.
  [RuBQ](https://github.com/vladislavneon/RuBQ) and
  [LC-QuAD 2.0](https://github.com/AskNowQA/LC-QuAD2.0) can broaden evaluation.
  Freebase property-frequency percentages do not establish Wikidata QA coverage.
- **Prior art informs the storage split.**
  [qEndpoint](https://github.com/the-qa-company/qEndpoint) and
  [HDT](https://github.com/rdfhdt/hdt-cpp) already combine dictionaries, immutable
  graph storage and indexed triple patterns. Reuse those as host-side options
  and evaluation references; a Java server is not a C33 runtime. No novelty
  claim about the absence of earlier embedded implementations is established.
- **Keep disk use bounded.** Download the dated JSON dump compressed (the
  September 7 bzip2 release is 103,048,420,178 bytes), stream it into selected
  staging tables, and never materialize the full uncompressed JSON. Download
  and staging tools reserve 24 GiB free. Full-import scratch and throughput
  still require measurement. Truthy RDF omits qualifiers/references, so it
  cannot replace full statements for historical joins; see
  [Wikidata's dump documentation](https://www.wikidata.org/wiki/Wikidata:Database_download).

The implemented first release includes a host builder, a shared C reader,
conservative factual templates, temporal/age/two-hop operations, an `ask ` mode
in the existing touchscreen UI, local article links, persistent question
History, and independent benchmark reporting. Optional superlatives, sentence
retrieval, embeddings and joke mode
remain later work. Full-corpus release validation follows the compressed dump
import; a small sample is not a substitute for that validation. Knowledge
ingestion uses sanctioned Wikimedia dump files only; API fetching has been
removed from the maintained workflow.

---

# Offline Wikipedia Question-Answering Appliance

Design notes for a factual QA device on a 60 MHz / 32 MB / 32-bit SoC with English Wikipedia on SD.

---

## 0. What we're building

A handheld, fully offline box you type a question into and get a correct factual answer out of, in well under a second, running on a CPU roughly as fast as a 1994 PalmPilot.

**The target experience:** you type `who was us president when the berlin wall fell`, and ~60 ms later it prints `George H. W. Bush` - along with the two hops it took to get there. Then you press a key and read the full Wikipedia article. No network, no model weights, no hallucination.

### The core bet

Small language models are bad at exactly the thing being asked of them here. Factual recall scales with parameter count, and nothing that fits this hardware knows anything. But **retrieval doesn't need parameters** - it needs precomputed indexes and a fast lookup path, and a 60 MHz chip is entirely adequate for that.

So the design splits knowledge from language and never lets them touch:

- **Knowledge** lives in ~4.4 GB of flat, precomputed index files on the SD card. All the intelligence was spent offline, on a real machine, at build time.
- **Language** is a dozen hand-written templates. There is no model on the device. There is no generation. The output is looked up and extracted, never composed.

This is deliberately not a chatbot, and shouldn't drift toward being one. The comparison point isn't ChatGPT - it's **Encarta 95**, which ran on a 486 with 8 MB and was genuinely useful precisely because it never lied to you.

### What success looks like

1. **Crisp answers to single-hop factual queries** - capitals, dates, heights, birthplaces, spouses. Fast enough to feel instant.
2. **A handful of multi-hop queries that look impossible** - temporal joins, chained relations, comparatives. These are the demo.
3. **Honest abstention.** The box says "no answer" and means it. This is the feature that makes everything else believable, and it's the one real advantage over a small neural model.
4. **Seamless navigation into the full corpus** - every answer entity is one keypress from its Wikipedia article, and articles link onward.

Realistic coverage is 25-40% of cold queries answered crisply. That's the honest number and the project is still worth building at that number.

### Build order

Each stage is independently demoable. Don't skip ahead - stage 1 validates the whole storage layout.

| Stage | Deliverable | Unlocks |
|---|---|---|
| 0 | **Done** - ZIM reader, exFAT extents, Xapian search | Storage layer is not a risk |
| 1 | Entity MPH + label store | `einstien` -> the right entity, on-device |
| 2 | Triple store + labels + relation lexicon | `capital of burkina faso` |
| 3 | Temporal table + 2-hop closure (strategy B) | **The chained demos - the actual product** |
| 4 | ZIM linking + keyboard UI + autocomplete | Feels like a product |
| 5 | Superlatives; sentence postings (optional) | Broader coverage, graceful fallback |
| 6 | Markov mode | The joke |

**All remaining risk is in the offline build pipeline** - data engineering on an unconstrained machine, plus a few hundred lines of lookup code atop a reader that already works.

**Build the ten demo queries' paths first, hardcoded, then generalize the templates.** Backwards from normal, but it front-loads the "does this feel like magic" question to day two instead of day ten.

### Notes to future me

- **The seek budget is the spec.** Any design question resolves by asking "how many SD seeks does this cost." 250 KB and 10 seeks per query. If a proposal breaks that, it's wrong regardless of how elegant it is.
- **Most of the capability is Wikidata, not the ZIM.** The ZIM only supplies lead text. Don't let the "Wikipedia on a chip" framing mislead the sourcing work.
- **Resist adding a parser to the device.** Flat files, `pread()`, pointer arithmetic. The moment SQLite or protobuf shows up, the design has failed.
- **Write the verification harness before flashing anything.** Binary offset bugs over UART are the thing most likely to kill momentum.
- **Property selection is upstream of everything.** If a Wikidata property isn't in the ~100 you chose, no amount of chaining or closure recovers it. Get this list right before building anything else.
- **Ship the abstain path early.** It's tempting to always return *something*. Don't.

---

## 1. Hardware constraints

| | |
|---|---|
| CPU | 60 MHz, 32-bit, **no hardware divide**, no FPU assumed |
| RAM | 32 MB SDRAM |
| Storage | SD card, **1.5 MB/s sequential**, ~1 ms per random seek (SPI mode) |
| Source data | English Wikipedia ZIM + optimized zstd |
| Build machine | Unlimited - all indexes precomputed offline |

### The binding constraint

**1.5 MB/s is the entire design.** Everything else is comfortable; 32 MB of RAM is palatial next to the 512 KB that microcontroller LLM projects fight over.

Per-query budget for a sub-500 ms answer:

- **~250 KB read**
- **~10 seeks**

Seeks matter as much as bytes. A 4 KB contiguous read costs barely more than 512 B, so:

> **Co-locate anything needed together. Precompute anything that would otherwise require a scan.**

### What this rules out

No transformer over retrieved context. Prefill on even a 3M-parameter model at 60 MHz is minutes per query. **The language model can never read the text it retrieves.** Knowledge and language must be separate systems, and the language system never sees the knowledge.

Consequence: the answer path is **lookup and extraction, not generation**.

---

## 2. Architecture

Router with a fixed set of paths, tried in order:

```
query
  -> normalize, strip stopwords
  -> resolve entity (MPH over titles + redirects)
  -> match remaining tokens against relation lexicon
       |- triple store            crisp,  ~20 ms
       |- materialized aggregate  crisp,  ~15 ms
       |- temporal join           crisp,  ~60 ms
       |- impact-ordered postings  hedged, ~150 ms
       |- binary embedding / IVF   hedged, ~350 ms
       `- "no answer"
```

**Ship the "no answer" path.** A system that answers 40% crisply and abstains on the rest is more impressive than one that always says something - and abstention is the one honest advantage this has over a small neural model.

### Key structures

**Entity resolution - one seek.** Wikipedia ships ~10M redirects alongside ~7M articles. Redirects are a hand-curated alias table: `JFK`, `Einstien`, `FDR`, `the Bard`. Normalize all 17M strings, build a minimal perfect hash offline (BBHash/CHD, ~2.5 bits/key ~ 5 MB), keep the MPH **resident in RAM**, store QID + 16-bit fingerprint on card. Typo and abbreviation tolerance for free, no fuzzy matcher.

**Triple store.** Sorted by `(subject, property)`, 8-byte fixed records, top ~100 Wikidata properties. A subject's whole claim block is 200-500 B - one seek. Keep the subject->offset table on card, not in RAM.

**Materialized superlatives.** Don't compute "tallest mountain in Japan" at runtime. Offline, enumerate every `(type x region x numeric property)` with >=3 members, store the top 10. ~500K combos x 10 x 8 B = 40 MB. **One seek.**

**Temporal officeholder table.** `(position, start_date, end_date) -> holder`. Every political office in history ~ 500K rows ~ 10 MB. This unlocks the impressive tier.

**Impact-ordered postings.** A term like `wife` has an 800 KB posting list - half a second just to read. Instead store a **head block per term: top 1,000 sentences by precomputed 8-bit impact score, 4 KB.** Caps per-term cost regardless of term frequency.

**Binary embeddings + IVF.** 7M leads x 48 B (384-dim, 1 bit/dim) = 336 MB. 8192 centroids resident (393 KB); probe 8 lists ~ 330 KB ~ 0.22 s. This is the `partner -> husband` rescue. SWAR popcount, ~12 ops/word.

### Chaining strategy - the main architectural decision

Multi-hop answers (`what language do they speak where machu picchu is`) can be resolved three ways. This is the choice that shapes the build.

| | **A. Runtime chaining** | **B. Precomputed chains** | **C. Fully materialized** |
|---|---|---|---|
| **On disk** | MPH + triples + temporal (~1 GB) | A + 2-hop closure (~16 GB) | B + phrasing table + rendered answers (~35 GB) |
| **Device does** | Loop the lookup, apply bridge rules | One seek per answer | Hash query -> print stored string |
| **Build time** | Weekend | +1 day (self-join, external sort) | +2-3 days |
| **On-device code** | ~600 lines | ~400 lines | ~200 lines |
| **Latency** | 40-80 ms | ~20 ms | ~15 ms |
| **Tuning loop** | Edit rules -> reflash -> test | Mostly build-time | Entirely build-time |
| **Coverage measurable pre-flash?** | No | Partly | Yes |
| **Unanticipated phrasings** | No | No | No |
| **Unanticipated entity/relation pairs** | Yes | No | No |

**The real tradeoff is where you debug.** A means iterating on-device over UART. B and C mean iterating on the build machine, where 2,000 golden queries run in a second and coverage is a number.

**Decision: B.**

A's flexibility is theoretical - bridge rules only fire for pairs you wrote rules for, so it covers no more ground than B, it just discovers failures later and in a worse place. C's extra 19 GB buys a small latency win and less device code, but moves brittleness into the *phrasing table*, which you can't verify against a golden set you also wrote. That undoes the main reason to materialize.

Keep A's runtime loop as a fallback for pairs outside the closure - ~100 lines, covers the long tail.

#### How each behaves

**A - works:** `capital of peru` -> Lima - `what language do they speak where machu picchu is` -> Spanish, Quechua, Aymara - `who succeeded churchill` -> Clement Attlee

**A - breaks:** `what currency do they use where the eiffel tower is` (no `currency@place` bridge rule; returns a bare claim block, indistinguishable from success until tested on-device) - `who was chancellor of germany when the wall fell` (office name resolves to the *article* about the position, not the temporal table's position ID) - `what continent is bolivia's capital on` (three hops; second lands on a city, and `continent` isn't a city property)

**B - works:** all of A's, plus `what currency do they use where the eiffel tower is` -> Euro - *because it was enumerated at build time and appeared in the coverage report* - `who was us president when the berlin wall fell` -> George H. W. Bush

**B - breaks:** `what continent is bolivia's capital on` (closure is 2 hops; falls to runtime loop and hits A's missing rule - but you knew before flashing) - `who was chancellor of germany when the wall fell` (if the closure only enumerated US positions, Germany is absent - visible as a gap in the build report) - `bolivia gdp per capita` (not a chain problem: if P2132 isn't in the ~100-property set, no closure helps - **property selection is upstream of everything**)

**C - works:** `whos bill clintons wife` -> Hillary Clinton (mangled possessive, no apostrophe) - `how tall is the eiffel tower` / `eiffel tower height` / `height of eiffel tower` -> all 330 m - `is tokyo bigger than london` -> Tokyo, by 5.1M

**C - breaks:** `is tokyo more populous than london` - same query, `populous` not enumerated, **zero coverage**. Brittleness is a cliff, not a slope - `what's the capital of the country machu picchu is in` (nested clause, no pattern match) - `bolivia population 1990` (temporal qualifier; rendered answers have one string per pair, no slot for "as of year X")

#### The asymmetry that decides it

- **A** fails on rules you didn't write - discovered on-device.
- **B** fails on the same rules - discovered in a build report.
- **C** fails on *words* you didn't write - discovered on-device, and unverifiable in advance.

---

## 3. Artifact sizes

| Artifact | Size | Source |
|---|---|---|
| 2-hop relation closure (~30 pairs) | 15.0 GB | derived - *strategy B* |
| Lead corpus, zstd + trained dictionary | 2.00 GB | ZIM |
| Triple store + subject index | 870 MB | Wikidata |
| Impact-ordered sentence postings | 600 MB | derived - *optional, see below* |
| Binary embeddings + IVF lists | 350 MB | derived (GPU) - *optional, see below* |
| Reverse index (~20 reversible props) | 200 MB | Wikidata |
| Entity MPH payload (17M keys) | 105 MB | Wikidata + redirects |
| Label store (dense-ID -> string) | 90 MB | Wikidata |
| Materialized superlatives | 60 MB | derived |
| Lead offset table | 35 MB | derived |
| ZIM entry index (link-out) | 28 MB | derived |
| Term dictionary | 20 MB | derived |
| Temporal officeholder table | 12 MB | Wikidata |
| **Total** | **~19.4 GB** | |

The base structures are ~4.4 GB; the 2-hop closure is the other 15. Alongside a 50 GB ZIM that's a 128 GB card - not a constraint worth optimizing against.

About a tenth of the `nopic` ZIM it's built from - you discard every sentence you'll never surface. Fits an 8 GB card with room for full article text.

Tight on space? Truncate leads to two sentences: 2.00 GB -> ~700 MB, negligible loss for answering.

### The two optional artifacts

Stock ZIMs ship a **built-in Xapian full-text index**, and the prod reader already searches it in seconds. That covers the fallback path for free, which makes ~950 MB of the table above skippable - and removes the GPU step, the only part of the pipeline needing hardware you may not have. Dropping both takes the build to **~3.4 GB**.

What Xapian doesn't give you:

- **Sentence granularity.** `bill clinton wife` works only because "bill", "clinton" and "wife" co-occur in one sentence of *Hillary Clinton*'s lead. Article-level BM25 loses that.
- **Vocabulary mismatch.** `partner` -> `husband` needs embeddings or an explicit expansion table; a stemmer won't bridge it.

**Recommended order:** build stages 1-4 with Xapian as the fallback, ship, then measure whether misses are vocabulary problems or template-coverage problems. Almost certainly the latter - in which case a ~500-entry synonym table buys most of what the 350 MB of embeddings would, and you never build them.

Of the two, **sentence postings are the more defensible to build**: cheap to derive, no GPU, and they enable a query class Xapian structurally cannot reach.

### RAM at runtime

| Resident | Size |
|---|---|
| MPH structure | 5.0 MB |
| LRU block cache | 8.0 MB |
| Roaring bitmap scratch | 4.0 MB |
| Answer buffers, working set | 1.0 MB |
| zstd dictionaries | 0.5 MB |
| IVF centroids | 0.4 MB |
| Relation lexicon, templates, stopwords | 0.2 MB |
| **Total** | **~19 MB** |

Spend spare RAM on the block cache. With repeated-topic usage the hit rate climbs fast and queries drop to single-digit milliseconds.

---

## 4. Build pipeline

### Sources

- **`latest-all.json.gz`** (Wikidata) - ~140 GB compressed, line-delimited JSON. Everything good comes from here.
- **`enwiki-latest-redirect.sql.gz`** + **`page.sql.gz`** - ~3 GB, gives the 10M alias strings.
- **The ZIM** - parse leads with `libzim`.

Build machine: ~1 TB scratch, 32 GB RAM, a GPU for the embedding pass. A weekend, dominated by the Wikidata scan.

> **Note:** most of the impressive capability is **Wikidata**, not the ZIM. The ZIM only supplies lead text for the extractive fallback.

### Steps

1. **Entity universe.** Stream Wikidata once, keep only entities with an `enwiki` sitelink (~7M of ~115M). Assign dense IDs 0..7M in sitelink-alphabetical order. Everything downstream uses these - turns lookups into array indexing, shrinks references to 4 bytes.
   *Don't use `jq`; it'll take days. Write a streaming `simdjson`/Rust parser - 4-8 hours, I/O bound.*
2. **Claims -> triple store.** Second pass, emit fixed-width records, external-sort by `(subject, property)`.
3. **Labels and aliases.** Third pass for English labels. Merge in redirects, normalize (lowercase, strip diacritics/punctuation, collapse whitespace), dedupe, build MPH. Store the fingerprint to reject out-of-vocabulary false positives.
4. **Leads.** Walk the ZIM, strip markup, sentence-split. **Train a zstd dictionary** on ~100K sampled leads (`zstd --train`), compress each lead independently against it. Without a dictionary 2 KB records compress terribly; with one you hit ~30-35%, so a lead is ~700 B - **two SD blocks.**
5. **Inverted index.** Stem, compute BM25 impact offline, quantize to 8 bits, sort descending, truncate to head blocks.
6. **Embeddings.** 7M leads through a small encoder on GPU, binarize by sign, k-means to 8192 centroids, group into IVF lists.
7. **Derived tables.** Superlatives = big group-by over the triple store. Temporal table from P39/P1308 claims with qualifier dates.

### Format discipline

**Flat files, absolute offsets, no pointers, no serialization framework.** Fixed header per artifact: magic, version, record count, section offsets. Device answers any query with `pread()` and pointer arithmetic. If you want protobuf or SQLite, you've added a parser to a 60 MHz chip for nothing.

**Pad hot structures to 512-byte boundaries** even at 20% size cost. Trading bytes for seeks, and seeks are what's scarce.

Little-endian, 4-byte aligned, with a `#define` for target endianness so build tool and firmware can't silently disagree.

### No-divide notes

- **BM25 has divisions.** Precompute quantized impact scores offline; runtime is adds only. Faster anyway.
- **Hash -> bucket:** multiply-shift, not modulo. Use `UMULL` and take the high word.
- **Dates:** int32 days-since-epoch, precomputed. Age = subtraction, then one fixed-point multiply by the reciprocal of 365.2425.
- **Any normalization:** force denominators to powers of two at build time so they become shifts.

### Verification

Build a golden set of ~2,000 query/answer pairs. Write a reference implementation **in Python reading the same binary artifacts**, run both, diff. Catches endianness bugs, off-by-one offsets, MPH collisions - all of which otherwise present on-device as "sometimes returns the wrong country" and are miserable to debug over UART.

---

## 5. Linking to ZIM articles

**Status: the storage layer already exists and works.** Stock ZIM files on exFAT, ~13 fragments, extent table cached at mount, boot and article loads in seconds. This section documents the constraints it operates under, not work to be done.

The join key is already there: step 1 kept only entities with an `enwiki` sitelink, and **the sitelink is the ZIM article title**.

Store a `zim_entry_index` (4 B) beside each entity's label - 28 MB, zero extra lookups.

### exFAT notes

`NoFatChain` (bit 1 of `GeneralSecondaryFlags`) is only set on fully contiguous files - it is mutually exclusive with a fragmented layout, so **don't assert on it**. The extent-table approach subsumes both: scan the FAT once at mount, cache the extents, and treat contiguous as the one-extent special case. 13 extents is ~104 bytes of RAM and a linear scan.

Read-only exFAT is a few hundred lines - boot sector, root directory cluster, walk 32-byte entry sets (`0x85` File, `0xC0` Stream Extension, `0xC1` Name) for known filenames, pull first cluster + length + flags. Don't link a general FAT library with write support.

At 1.5 MB/s you're on SPI: budget **2-3 ms per random read**, not 1. Use CMD18 multi-block for anything >=4 KB; repeated CMD17 wastes most of the transaction on command overhead.

### Article reads with stock clusters

Kiwix clusters are ~1-2 MB uncompressed (hundreds of articles batched for compression ratio), so extracting one 8 KB article means decoding into a big cluster. **Streaming decode with early exit** is the working approach:

| Step | Cost |
|---|---|
| URL pointer list -> cluster + blob index | 2 seeks |
| Read cluster, early exit at blob end (~50% avg of 1 MB) | ~0.35 s |
| zstd decode at ~2-5 MB/s on a 60 MHz core | ~0.15-0.35 s |
| **Total** | **~0.5-0.8 s** |

Two details: the blob offset table lives at the *head* of the decompressed cluster (first byte flags 32- vs 64-bit offsets), so decode the head, learn the target, continue, stop. And budget **1-2 MB for the zstd window** - the output ring can't be smaller than whatever window the frame header declares.

Repacking article bodies into per-article zstd records would give single-seek access, but it's unnecessary given measured performance and it means abandoning stock ZIMs. Not worth it.

The lead corpus artifact is unaffected either way - that's extracted at build time into its own file. Stock ZIM only constrains full-article reads.

### Boot

Everything except the MPH is small. 5.3 MB of MPH is 3.5 s of pure transfer on its own. **Tiered load:** boot the ~900 KB critical set (extent tables, zstd dictionaries, IVF centroids, artifact headers) in ~0.6 s, then stream the MPH in the background while the user types. If a query arrives before it's resident, fall through to on-card BBHash lookup (3-4 levels = 3-4 seeks, ~10 ms). No "loading" state ever appears.

**Gotcha:** ZIM link targets are URL-encoded namespaced paths (`A/Pierre_Curie`), not raw titles. Normalize identically to the MPH keys - strip namespace, percent-decode, underscores to spaces - or you get maddening misses on exactly the articles with punctuation in their names.

---

## 6. Query examples

### Triple store - crisp, ~20 ms

```
capital of burkina faso
  Burkina Faso -> capital -> Ouagadougou

einstien birthplace
  Albert Einstein -> place of birth -> Ulm
  (matched via redirect alias "Einstien")

how tall is the eiffel tower
  Eiffel Tower -> height -> 330 m
```

### Derived tables - crisp

```
tallest mountain in japan
  Mount Fuji - 3,776 m
  2. Mount Kita 3,193 m   3. Mount Okuhotaka 3,190 m

how old was mozart when he died
  Wolfgang Amadeus Mozart - 35 years
  b. 1756-01-27  d. 1791-12-05

who was us president when the berlin wall fell
  Berlin Wall -> dissolved -> 1989-11-09
  President of the United States @ 1989-11-09 -> George H. W. Bush

who succeeded churchill
  Winston Churchill -> Clement Attlee (1945-07-26)
```

The Berlin Wall query is **the demo**: two-hop temporal join, the canonical "surely this needs an LLM" question, answered in 60 ms on a 60 MHz chip.

### Multi-hop chains

```
what language do they speak where machu picchu is
  Machu Picchu -> country -> Peru
  Peru -> official language -> Spanish, Quechua, Aymara

is tokyo bigger than london
  Tokyo      13,988,129
  London      8,866,180
  -> Tokyo, by 5.1M
```

Three languages for Peru is correct and more honest than a chatbot would be - it prints all claims rather than picking one. For comparisons, **print the claim date and unit**: Wikidata population is city-proper, and metro-area would flip this. That's the difference between an answer and a citation.

### Reverse index

```
countries that border bolivia
  Brazil - Peru - Chile - Argentina - Paraguay
  (5 results - P47 reverse)
```

### Fallbacks - hedged with `~`

```
marie curie partner
  no relation match for "partner"
  ~ semantic search:

  Marie Curie - "...she shared the 1903 Nobel Prize in
  Physics with her husband Pierre Curie..."
  [ENTER] Marie Curie   [->] Pierre Curie

why did the roman empire fall
  ~ Fall of the Western Roman Empire
  "...the loss of centralized political control over the
  West, and the rise of successor kingdoms, is attributed
  to military failures, fiscal pressure, and the arrival
  of migrating peoples."
  [ENTER] read article
```

### The abstain - include this in the demo

```
who would win a fight between a bear and a shark
  no answer
```

Showing the abstain path is what makes the correct answers credible. Without it, an observer can't tell the machine isn't just producing confident-sounding text - precisely what small neural models do and this doesn't.

---

## 7. Interface

```
Marie Curie -> spouse -> Pierre Curie

  Pierre Curie (1859-1906) was a French physicist, a
  pioneer in crystallography, magnetism, piezoelectricity
  and radioactivity.

  [ENTER] read article   [->] Marie Curie   [/] new query
```

**Show your work.** Printing `subject -> relation -> object` costs three lines of code, makes wrong answers diagnosable instead of mysterious, and reads as a system with a mechanism rather than an oracle.

**Navigable entities on both sides.** Free from the triple store - subject and object are both entity IDs, both have ZIM entries, both one keypress away. Once inside an article, ZIM internal links resolve through the same MPH. Wikipedia-style link-following, offline, at 60 MHz.

**Entity autocomplete as you type.** Add a sorted prefix index over the MPH keys, offer completions after ~3 characters. Kills the most common failure (unresolvable entity) before submission, and watching a handheld complete obscure entity names off an SD card is the moment it stops looking like a toy.

---

## 8. Honest expectations

**Coverage.** Cold user with a keyboard: roughly **25-40% crisp correct**, ~20% relevant-but-not-an-answer, rest abstain or miss. Good for the hardware. Not a chatbot.

**It's a router, not a parser.** A dozen hand-built templates matching anticipated query shapes. `capital of X` works because you wrote that rule. The gap between "these ten queries work" and "queries like these work" is the whole difficulty, and it's breadth of pattern coverage - manual work, not cleverness.

**Widening coverage cheaply:**
- **Mine public query logs** (AOL/MSN) offline for real phrasings. Most factual queries collapse into ~50 patterns, and `X wife` (bare, no verb, no question mark) is far more common than `who is the wife of X`.
- **Bag-of-relations, not grammar.** Strip stopwords, resolve one entity, match remaining tokens against the relation lexicon. Handles word-order variation for free - `bill clinton wife`, `wife of bill clinton`, and `whos bill clintons wife` all land the same.

**Known failure modes:**
- **Vocabulary mismatch** - `partner` vs `husband`. The IVF embeddings exist for this; an explicit expansion table (~500 entries) covers only what you anticipated.
- **Ambiguous relation mapping** - `partner` could map to P26 (spouse) or P451 (unmarried partner). Mapping `marie curie partner` to P451 returns Paul Langevin: sourced, defensible, and completely the wrong answer to a casual question. A system with no pragmatics can't tell that the default reading is meant.
- **Entity collisions** - "Marie Curie" is also a UK end-of-life charity, which has a *corporate partners* page. Both query terms match strongly and literally.
- **Multi-hop reasoning beyond the templates** - anything needing a join you didn't precompute.

**The character of the machine:** it answers a two-hop temporal join instantly, then fails on something a child could answer, with no warning. That discontinuity is the real texture of it - and the failures are **legible**. You can look at a wrong answer and say "ah, P451" or "ah, the charity." Far nicer than a 1B model smoothly inventing a plausible spouse and giving you no way to tell.

---

## 9. Optional: the joke generator

Keep a Markov-chain mode on a **separate command**, not as a fallback.

**A 4-gram over 2 KB is too high-order to be funny.** With ~350 words of source, almost every 3-word prefix has exactly one continuation, so the chain regurgitates the article verbatim. **Order is the comedy dial:**

*4-gram, single article* - near-verbatim, splices once:

> Bolivia, officially the Plurinational State of Bolivia, is a landlocked country located in western-central South America. The constitutional capital is Sucre, while the seat of government and financial center is located in Bolivia, is a landlocked country located in western-central South America.

*3-gram* - splices every sentence or two, still competent:

> Bolivia is a landlocked country in western-central South America. The constitutional capital is Sucre, while the seat of government is the largest city and the country is bordered by Brazil to the north and east, and shares a border with Peru to the west.

*2-gram* - grammatical, confident, semantically unmoored:

> Bolivia is bordered by Brazil to the seat of government is Sucre, while the Andes mountain range spans the constitutional capital is a landlocked country located in western-central South America and the largest city.

**2-gram blended across the top 3-5 retrieved articles** - the sweet spot, because you get topic collision:

> Marie Curie was a French physicist who shared the 1903 Nobel Prize in Physics with her husband Pierre Curie, who was crushed by a horse-drawn cart and is awarded annually to the person who shall have conferred the greatest benefit to radioactivity.

Starts true, stays grammatical, arrives somewhere insane. Seed from a query term and you get that arc for free - early steps follow high-count paths, drift accumulates.

### Implementation

- **Cycles.** Bigram chains love `of the government of the government of the`. Cap repeat visits to any state at 2 and force a restart, or you'll hit the length limit every time.
- **No divide.** Weighted sampling needs a cumulative-sum table built at insert time, then `(rng32 * total) >> 32` for an index - no modulo.
- **Punctuation as first-class tokens.** Keep `.` `,` `(` in the chain or sentence structure collapses and it stops being funny, just word salad. Terminate on `.` plus a ~40-token cap.

Memory is nothing: 5 articles ~ 1,800 words ~ 1,800 bigram entries in a small open-addressed hash table, well under 100 KB. Build per query, throw away.

Print the seed so jokes are reproducible:

```
> /babble marie curie
  [seed 0x4A21 - 4 articles - bigram]
  Marie Curie was a French physicist who shared...
```

### If you want an actual neural model

Reference points for conversational padding:

- **TinyStories 260K on Cortex-M7**: ~87 tok/s at 280 MHz, ~1,077 KiB flash + 916 KiB SRAM, on-chip only. Scaled to 60 MHz: ~15-20 tok/s.
- **Ternary weights** (BitNet-style `ternary15M`): all linear layers in {-1, 0, +1}. On a chip with no FPU, every multiply becomes an add or a skip.
- **Per-Layer Embeddings**: 28.9M params on an ESP32-S3 at 9.88 tok/s, where 25M live in a flash-mapped lookup table and the dense core is only 556K. The constraint is fast memory, not total memory - exactly this situation.

None of them know anything. That's what the 4.4 GB of indexes are for.
