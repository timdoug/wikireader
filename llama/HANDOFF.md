# Llama on the WikiReader — where this stands

Written 2026-09-20. `llama/README.md` documents the app as built;
`llama/train/README.md` documents the corpus pipeline. This file is the
part that is opinion rather than record: what the measurements imply, and
what I would and would not build next.

## What works

`llama/` runs Llama 2 on the device. stories260K generates at **119 ms a
token**, with no floating point anywhere in the forward pass, and agrees
with an fp32 reference on the top-1 prediction at every teacher-forced
position (45 of 45). Temperature sampling, an on-screen keyboard and
prompt entry all work. `make test` covers arithmetic, generated text,
prompt round-tripping, sampling and a truncated-file rejection.

Commits: `d0c17b7b` (port), `78bbb99e` (fixed point, 10x), `462254ce`,
`5cc7cd6e`, `b7fcb1a5` (emulator ergonomics and the loop alignment),
`4ddda0f8` (stop reasons), `bea2a097` (keyboard), `cd1e236f` (temperature),
`742b6e1b` (corpus and vocabulary).

## The three numbers that decide everything

**17.2 cycles per multiply-accumulate.** Measured, not modelled. Every
projection below follows from it: seconds a token is about
`MACs * 4.6e-7`, which reproduces the measured 125 ms for stories260K.

**2.02 tokens a word** on Simple English at a 4096-entry vocabulary,
against about 1.3 for Llama's 32000-entry vocabulary on ordinary English.
A small vocabulary spells proper nouns out and an encyclopedia is made of
them. This doubles the cost of a word and is the single most important
number for sizing a model; I had assumed 1.5 and was wrong.

**The card is 990 MB and exact; RAM is 32 MB; the CPU is 60 MHz.** The
asymmetry is three orders of magnitude and it should decide the
architecture of anything built here.

## What a model can be on this part

| dim / layers / ctx | params | s/token | words/min |
| --- | ---: | ---: | ---: |
| 64 / 4 / 256 | 0.45 M | 0.27 | 110 |
| 96 / 6 / 256 | 1.06 M | 0.62 | 48 |
| 128 / 8 / 512 | 2.13 M | 1.47 | 20 |
| 192 / 8 / 512 | 4.33 M | 2.73 | 11 |

Two things to take from this. Human reading is about 200 words a minute,
so **nothing here keeps up**. And the 32 MB board would hold a
27M-parameter model, which takes 14 seconds a token — **patience runs out
about ten times before memory does**, so anything above roughly 3 MB of
weights is not worth building. The RAM is not the constraint and never was.

The vocabulary is also not the lever it is for stories15M. Measured, 4096
against 8192 on Simple English is 2.96 against 3.12 seconds a word: the
shorter sequence and the longer classifier cancel. 4096 wins on size alone.

## Why I would not ship a generative model

A 1.06M-parameter model is **1.06 MB of weights against 64 MB of
entropy-coded corpus** — 61x smaller than the information it is trained
on, and it has to discard about 98% of what it sees. What survives is
grammar, article shape and which kinds of words co-occur. Not which fact
attaches to which name.

So the expected output is fluent encyclopedic English with invented
contents: the right category and the wrong instance, plausible-shaped
dates, entity drift partway through, confabulated populations and titles.
TinyStories works at this scale because its domain is deliberately narrow.
Simple English Wikipedia is simpler *prose* but not a narrower *domain*.

And the device already carries the same knowledge, verbatim, at a thousand
times the capacity. A model that hallucinates is strictly worse than the
search already shipping, and `sparrow/` already answers factual questions
by retrieval.

## What I would build instead

**Put the knowledge on the card and give the model only the question.**

Retrieval by meaning, which the reader cannot currently do at all — it
searches titles and prefixes, so "that country with the maple leaf flag"
finds nothing. Costed on this hardware:

| | |
| --- | ---: |
| 64-dim int8 embedding per article, 244k articles | 15.6 MB on the card |
| brute force scan | 17.4 s card + 4.5 s compute — no |
| **inverted file, 512 centroids** | **52 ms** |
| centroid table, resident | 32 KB |
| one cluster read from the card | 30 KB |
| query encoder as averaged word vectors | 256 KB |

The inverted file is what makes it work: score 512 centroids in RAM, then
read only the matching cluster from the card. And the query encoder can be
averaged word vectors rather than a transformer — a 4096 x 64 int8 table,
and encoding a query is a few vector adds, so there is no forward pass in
the interactive path at all.

Then **extract rather than generate**: score the retrieved article's
sentences against the same query vector and show the best three. The
answer is quoted text, so it is true by construction, and it uses the same
machinery. This is the design I would pursue.

Ranked by usefulness per cycle on this part:

1. semantic retrieval plus sentence extraction — 52 ms, cannot hallucinate
2. "did you mean" over the title index — edit distance, no model needed
3. generation — 48 words a minute and invents its facts

The 1M model still has two honest uses: it is what you train to get
embeddings out of (mean-pooled hidden states, or distilled to word
vectors), and it demonstrates that this part runs a transformer at all,
which is why all of the above is measured rather than guessed.

## Corpus and training, as left

`build/simple.txt` is 244,114 articles, 263 MB of ASCII, 88.0M tokens at
vocab 4096, pretokenized into 20 shards under `build/pretok/tok4096`
(shard 00 is the validation split). `build/tok4096.{model,vocab,bin}` is
the vocabulary; the `.bin` is in the format `llama/tokenizer.c` reads.
`build/llama-venv` has libzim, sentencepiece and torch. None of this is in
git — `build/` is ignored — so it is all reproducible from
`llama/train/README.md` and nothing is lost by deleting it.

Training on the M2 works and is verified only as a smoke test: loss 8.30
to 6.60 over 40 iterations, about 3.6 s an iteration at 65,536 tokens an
iteration, which is **18k tokens a second, or 80 minutes an epoch**. A
useful run is 4 to 6 epochs, so **6 to 8 hours** — an overnight job, not a
multi-day one. The command is in the README; note that piping it through
`head` kills it on SIGPIPE, which is how the first attempt died silently.

MPS reaches about 0.05% of model-FLOPs utilization here. That is not a
mistake in the setup: the matmuls are 96x96 and the GPU spends its time on
kernel launches. Batch 64 measured fastest; 128 and above were slower.

## Open questions I did not resolve

- Whether embeddings from a 1M generative model beat plain word vectors
  enough to justify training one. Word vectors alone may be sufficient for
  topical retrieval, in which case no transformer is needed on the device.
- How to build the inverted file: k-means over 244k embeddings is a host
  job, but the cluster assignment has to survive into the card layout, and
  `zim/` has no host-side tooling to write alongside an archive.
- Whether the 27-byte fetch window bites the embedding scan the way it bit
  the matmul. `-falign-loops=16` on `model.o` is load-bearing there and the
  same care will be needed for any new hot loop.
