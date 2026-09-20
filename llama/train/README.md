# Training a model for this device

Everything here runs on the host. It takes a ZIM archive to a corpus and a
vocabulary; the model itself needs a GPU and llama2.c's `train.py`.

```sh
python3 -m venv build/llama-venv
build/llama-venv/bin/pip install libzim sentencepiece

build/llama-venv/bin/python llama/train/extract.py \
    wikipedia_en-simple_all_nopic_2026-06.zim build/simple.txt
build/llama-venv/bin/python llama/train/tokenizer.py \
    build/simple.txt build/tok4096 --vocab-size 4096
```

Extraction is about 22 minutes for Simple English, the vocabulary about 5.

## What Simple English gives you

| | |
| --- | ---: |
| articles kept | 244,114 |
| characters, ASCII | 263 M |
| tokens at vocab 4096 | **88 M** |
| chars a token | 3.01 |
| **tokens a word** | **2.02** |
| mean article | 348 tokens |

Dropped on the way: redirects, stubs under 200 characters, and 6,148 date
and year pages -- "June 22", "1932" -- which are birth-and-death lists, 5%
of the archive by volume and almost entirely one sentence with the numbers
changed. A model of a few million parameters learns that form very well and
it is not worth the capacity.

The markup this skips is the markup `zim/zim_html.c` skips:
`class_word_skipped` there is the source of the class list, so the training
text is the text the device would have shown.

**The corpus is folded to ASCII.** It had 7,695 distinct characters, almost
all from one-off glosses like "(Arabic: ...)" beside a name, and
sentencepiece refused a 4096-entry vocabulary because the character set
alone needed 7,946. The panel's font is 8x13 with 256 entries and cannot
draw those scripts whatever the model does, so folding costs 0.33% of the
text and buys the vocabulary size back.

## What fits, and how slow it is

At the measured 17.2 cycles a multiply-accumulate, 2.02 tokens a word:

| dim / layers / context | params | s/token | words/min | 50-word paragraph |
| --- | ---: | ---: | ---: | ---: |
| 64 / 4 / 256 | 0.45 M | 0.27 | 110 | 0.5 min |
| 64 / 6 / 256 | 0.54 M | 0.34 | 87 | 0.6 min |
| **96 / 6 / 256** | **1.06 M** | **0.62** | **48** | **1.1 min** |
| 128 / 6 / 256 | 1.73 M | 0.98 | 30 | 1.7 min |
| 128 / 8 / 512 | 2.13 M | 1.47 | 20 | 2.5 min |
| 192 / 8 / 512 | 4.33 M | 2.73 | 11 | 4.6 min |

Human reading is about 200 words a minute, so none of these keep up. The
32 MB board would hold 26 MB of weights -- a 27M-parameter model -- but
that takes 14 seconds a token, so **patience runs out about ten times
before memory does**. Nothing above about 3 MB of weights is worth
building.

## The vocabulary is not the lever it was for TinyStories

4096 against 8192, same model:

| vocab | tokens a word | weights (d=128, L=8) | s/token | s/word |
| --- | ---: | ---: | ---: | ---: |
| 4096 | 2.02 | 2.1 MB | 1.47 | **2.96** |
| 8192 | 1.82 | 2.7 MB | 1.71 | 3.12 |

A bigger vocabulary shortens the sequence and lengthens the classifier, and
on Simple English those cancel almost exactly. 4096 wins on size, not
speed. (For stories15M the same question is not close: its 32000-entry
classifier is 60% of its per-token work.)

Note 2.02 tokens a word, against about 1.3 for Llama's 32000-entry
vocabulary on ordinary English. Simple Wikipedia is full of proper nouns,
and a small vocabulary spells them out.

## What to expect from the output

Fluent, encyclopedic-sounding English with unreliable facts. TinyStories
works at one to ten million parameters because the domain is deliberately
narrow -- toddler vocabulary, simple plots. Simple English Wikipedia is
simpler *prose* but not a narrow *domain*: every topic, dense with names
and dates. A model of this size will learn the shape of an article and
invent the contents.

There is also 88M tokens of training data against a compute-optimal size of
about 4.4M parameters, so the sizes that run fast enough to use are well
inside what the data supports -- which is the right regime for a model that
will be run many times and trained once, and is what TinyStories did.

## Next: the model

`train.py` from llama2.c, then `export.py --version 0`, then
`llama/tools/convert.py`. A model this small is hours on one consumer GPU,
not days.
