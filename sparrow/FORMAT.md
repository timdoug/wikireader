# Sparrow binary format v1 (provisional)

All integers are little-endian; readers decode bytes explicitly. Offsets are
absolute unsigned 64-bit byte offsets. No native structs are serialized.
All data blocks start at 512-byte boundaries. Directories have 16-byte entries:
`offset:u64, length:u32, crc32:u32`. CRC uses CRC-32/ISO-HDLC (zlib convention).
Unused/padding bytes are zero. A CRC detects damage, not malicious forgery.

The header is 128 bytes:

| Offset | Field |
| --- | --- |
| 0 | `SPRWQA1\0` (8 bytes) |
| 8 | version u32 = 1 |
| 12 | header length u32 = 128 |
| 16 | total file length u64 |
| 24 | alias bucket count u32, positive power of two |
| 28 | entity count u32 |
| 32 | alias directory offset u64 = 512 |
| 40 | entity directory offset u64 |
| 48 | source snapshot identity, 32 bytes, NUL terminated |
| 80 | CRC-32 over bytes 0..79 |
| 84 | reserved zero bytes through 127 |

Alias buckets contain repeated `entity:u32, key_length:u16, key_bytes` records.
Keys are UTF-8, at most 127 bytes. Normalize ASCII uppercase to lowercase,
underscores to spaces, collapse ASCII space/tab/CR/LF, and trim spaces. No
Unicode accent stripping or case folding occurs. Hash UTF-8 with FNV-1a32,
then the xor/multiply avalanche in `sparrow.c`; use `hash & (bucket_count-1)`.
The builder verifies that every bucket fits 4096 bytes. Full key comparison is
required. The entity value `0xffffffff` means an ambiguous alias, never an
entity. Duplicate aliases for the same entity coalesce before building.
A unique normalized English Wikipedia article title takes precedence over
other entities' labels and aliases. If two article titles normalize to the same
key, that key remains ambiguous. Other alias collisions still abstain.

Entity IDs are dense, zero-based, assigned by ascending numeric QID. The
entity directory is on card. Each block (at most 65536 bytes) begins with:
`qid:u32, label_length:u16, enwiki_title_length:u16, claim_count:u16, reserved:u16`,
followed by the label and title bytes, without NULs. Strings are UTF-8, at most
255 bytes. Empty titles denote entities without a local-article join key.
The builder uses the numeric QID as the label when a label exceeds this bound,
and omits oversized article titles. Both cases are counted in the manifest.

Each claim has a 36-byte fixed header, followed by three strings:

| Offset | Field |
| --- | --- |
| 0 | property u16 (65534 is the derived officeholder relation) |
| 2 | kind u8: item=1, time=2, quantity=3, string=4 |
| 3 | rank u8: normal=0, preferred=1 |
| 4 | object dense ID u32, or `0xffffffff` |
| 8 | exact Gregorian date i32 |
| 12 | start date i32 |
| 16 | end date i32 |
| 20 | point-in-time date i32 |
| 24 | flags u16: unsafe=1, start qualifier=2, end=4, as-of=8, partial qualifier date=16, unresolved item metadata=32 |
| 26 | displayed value length u16 |
| 28 | unit label length u16 |
| 30 | statement ID length u16 |
| 32 | object numeric QID u32, or zero for literals |
| 36 | displayed value, unit, statement ID UTF-8 bytes |

Dates use positive `YYYYMMDD`, with zero meaning unavailable for arithmetic.
Only exact Gregorian days in years 1..9999 enter arithmetic or temporal joins.
Other time precision/calendar values remain display text.
Year/month precision in qualifier fields uses `YYYY0000`/`YYYYMM00` and sets
flag 16. Such dates are rendered at their stored precision. An office term
that could overlap the query day still needs exact endpoints; imprecise dates
can only exclude a definitely disjoint term. Earlier readers reject flag 16,
so update firmware together with indexes that use this extension.

Flag 32 means an item statement has its numeric object QID but no corresponding
entity metadata in this index. Its dense ID is `0xffffffff` and its fallback
display is the QID. Counting distinct recorded QIDs and proving that a relation
exists do not need that metadata. Ordinary value lookup and traversal still
abstain. Flag 1 independently records unsupported qualifiers or values and
always prevents these aggregates. Earlier readers reject flag 32; install the
matching firmware with a new index. Older indexes that used flag 1 for missing
metadata remain readable and conservatively abstain in that case.

Original JSON
statements (including references and all qualifiers) are retained in the host
`.statements.jsonl.gz` audit; qualifiers not understood by the reader's rules
set `unsafe`, making the property abstain. Units and decimal amounts are never
converted through floating point.

When the entity block would exceed 65536 bytes or 65535 claims, the builder
replaces its largest property group with one preferred-rank, unsafe string
record, repeating until the block fits. It never retains a truncated subset
as an answer. The host audit keeps all original claims; the manifest counts
omitted device claims and replaced property groups. Other properties in that
entity remain usable.

This is a purpose-built selected-claim artifact, not an RDF/HDT interchange
format. The source dump, snapshot, audit and build manifest remain necessary
for rebuilding, interpretation, and independent evaluation.
