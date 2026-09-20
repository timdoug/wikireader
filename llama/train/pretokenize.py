#!/usr/bin/env python3
"""Encode the extracted corpus into the shards llama2.c's train.py reads.

A shard is a flat uint16 array of token ids with no record structure:
`PretokDataset` memory-maps it and cuts `max_seq_len` windows out of it at
random offsets, so documents are separated only by the BOS that starts each
one. Shard 0 is held out -- train.py uses it as the validation split and
the rest for training.

uint16 caps the vocabulary at 65536, which is far above anything this
device can afford to run: the classifier is a `dim x vocab` matmul per
token, so 4096 is already a tenth of the model's work.
"""

import argparse
from pathlib import Path

import numpy as np


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("text", type=Path, help="extract.py output")
    ap.add_argument("model", type=Path, help="a .model from tokenizer.py")
    ap.add_argument("out", type=Path, help="directory for the shards")
    ap.add_argument("--shards", type=int, default=20)
    args = ap.parse_args()

    import sentencepiece as spm

    sp = spm.SentencePieceProcessor(model_file=str(args.model))
    if sp.get_piece_size() > 65536:
        raise SystemExit("vocabulary does not fit uint16")

    docs = [d for d in args.text.read_text(encoding="utf-8").split("\n\n\n")
            if d.strip()]
    print(f"{len(docs)} documents")

    args.out.mkdir(parents=True, exist_ok=True)
    per = (len(docs) + args.shards - 1) // args.shards
    total = 0
    for s in range(args.shards):
        chunk = docs[s * per:(s + 1) * per]
        if not chunk:
            break
        ids = []
        # BOS in front of every document and no EOS, which is what
        # llama2.c's own pretokenizer does: a document boundary is where
        # the model sees a token it only ever sees there.
        for piece in sp.encode(chunk, add_bos=True, add_eos=False):
            ids.extend(piece)
        arr = np.array(ids, dtype=np.uint16)
        path = args.out / f"data{s:02d}.bin"
        arr.tofile(path)
        total += len(arr)
        print(f"  {path.name}: {len(chunk)} docs, {len(arr)/1e6:.2f}M tokens")
    print(f"{total/1e6:.1f}M tokens total; shard 00 is the validation split")


if __name__ == "__main__":
    main()
