# Sparrow design

Why Sparrow is built the way it is. For the binary layout see [FORMAT.md](FORMAT.md),
for usage [README.md](README.md), for measurements [BENCHMARKS.md](BENCHMARKS.md).

## Knowledge and language never touch

Factual recall scales with parameter count, and nothing that fits a 32 MB device
knows anything worth asking. Retrieval does not need parameters - it needs
precomputed indexes and a fast lookup path, which 60 MHz is adequate for.

So knowledge lives in flat, immutable index files on the SD card, built offline
on a real machine, and language is a set of hand-written templates. There is no
model on the device and no generation: output is looked up and extracted, never
composed. The comparison point is a good offline encyclopedia, not a chatbot.

## Honest abstention is the feature

A lookup can still be wrong. A 16-bit fingerprint admits false positives,
aliases collide, and source claims can be incomplete or inconsistent. Sparrow
therefore verifies aliases exactly, CRC-protects blocks, and reports ambiguity
rather than guessing. What it promises is traceable recorded claims, not
infallibility - a missing fact does not prove false or zero, and counts are
explicitly of recorded distinct QIDs.

Abstention is what makes the answers believable, and it is the one real
advantage over a small neural model.

## Wikidata semantics are retained

Eight-byte triples cannot represent ranks, unknown values, units, precision,
scope or qualifier dates, so Sparrow does not use them. It preserves ranks,
qualifiers, units, date precision, calendars and original statement IDs, and
abstains on claim semantics it cannot represent. Entities without an enwiki
sitelink are still included: dropping them would lose units, offices and bridge
nodes. See the [Wikibase JSON model](https://doc.wikimedia.org/Wikibase/master/php/docs_topics_json.html).

## Runtime joins, measured before anything is materialized

The same portable C runs on the host and on C33, so runtime joins can be tested
off-device as thoroughly as materialized ones. A large precomputed closure has
to demonstrate measured benefit before it earns its size. Measure real SD reads,
transfer bytes and CPU/render time; logical reads are not SD seeks, and a 4 KiB
transfer is not free compared with 512 bytes at this bandwidth.

The main remaining capability gap is the reverse direction - finding subjects
from an object, then filtering and intersecting candidates. A host-built
`(property, object)` posting list could cover it without device-wide scans, but
index size and candidate fan-out need measuring first.

## Sanctioned dumps only

Knowledge comes from published Wikimedia dump files: no entity API, no SPARQL
endpoint, no scraping. The dated JSON dump is streamed compressed into selected
staging tables; the full uncompressed JSON (roughly 1.8 TB) is never
materialized, and the tooling holds a 24 GiB free-space reserve. Truthy RDF
omits qualifiers and references, so it cannot substitute for full statements on
historical joins. See [Wikidata's dump documentation](https://www.wikidata.org/wiki/Wikidata:Database_download).

Articles come from the local Wikipedia ZIM through the existing reader.
`zim/search.c` does title-prefix search; there is no embedded full-text index
to fall back on.

## Hardware

The target is the tested **32 MB, 60 MHz Epson C33** units. The existing reader
already consumes substantial RAM, so Sparrow's budget is accounted on top of it
with bounded buffers and a reused file cache. This is C33, not ARM - it has
multiply and step-division instructions, so avoiding unnecessary division is
worthwhile but banning division is not.

## Evaluation

Synthetic regressions cover binary corruption and semantic boundaries; they test
the implementation and do not count as question-answering coverage.

Independent evaluation uses held-out questions from
[QALD-9-plus](https://github.com/KGQA/QALD_9_plus) (371 Wikidata train / 136
test). [RuBQ](https://github.com/vladislavneon/RuBQ) and
[LC-QuAD 2.0](https://github.com/AskNowQA/LC-QuAD2.0) can broaden it. Rules are
developed on training questions with code and data frozen before test runs;
gold answers and gold-query execution stay out of knowledge ingestion.

## Prior art

[qEndpoint](https://github.com/the-qa-company/qEndpoint) and
[HDT](https://github.com/rdfhdt/hdt-cpp) already combine dictionaries, immutable
graph storage and indexed triple patterns, and are useful as host-side options
and evaluation references. A Java server is not a C33 runtime, but nothing here
claims to be the first embedded implementation of the idea.
