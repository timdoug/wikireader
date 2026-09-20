#!/usr/bin/env python3
"""Train a BPE vocabulary on extracted text and write it in the device's
format.

Vocabulary size is a performance decision here, not just a quality one.
The classifier is a `dim x vocab` matmul run once per token, so its cost is
`dim * vocab` multiply-accumulates -- at dim 128 a 32000-entry vocabulary
costs 4.1M of those, against 1.6M for the entire rest of the model. It has
to be small, and llama2.c's own notes say a 4096-entry vocabulary trained
on narrow text gives about the same sequence lengths as Llama's 32000-entry
one, because a tokenizer fitted to the corpus does not waste entries.

The binary layout is llama2.c's `tokenizer.bin`, which `llama/tokenizer.c`
reads unchanged: a leading int for the longest piece, then for each token a
float score, an int length, and that many bytes.
"""

import argparse
import struct
from pathlib import Path


def export_bin(model_prefix, out_path):
    import sentencepiece as spm

    sp = spm.SentencePieceProcessor(model_file=f"{model_prefix}.model")
    tokens, scores = [], []
    for i in range(sp.get_piece_size()):
        piece = sp.id_to_piece(i)
        score = sp.get_score(i)
        if i == sp.bos_id():
            piece = "\n<s>\n"
        elif i == sp.eos_id():
            piece = "\n</s>\n"
        # SentencePiece marks a word boundary with U+2581; the device
        # wants the space itself.
        piece = piece.replace("▁", " ")
        tokens.append(piece.encode("utf-8"))
        scores.append(score)

    with open(out_path, "wb") as f:
        f.write(struct.pack("<i", max(len(t) for t in tokens)))
        for token, score in zip(tokens, scores):
            f.write(struct.pack("<fi", score, len(token)))
            f.write(token)
    return len(tokens)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("text", type=Path, help="extract.py output")
    ap.add_argument("prefix", type=Path, help="output prefix")
    ap.add_argument("--vocab-size", type=int, default=4096)
    ap.add_argument("--input-sentence-size", type=int, default=4000000,
                    help="sentences sampled for training the vocabulary")
    args = ap.parse_args()

    import sentencepiece as spm

    args.prefix.parent.mkdir(parents=True, exist_ok=True)
    spm.SentencePieceTrainer.train(
        input=str(args.text),
        model_prefix=str(args.prefix),
        model_type="bpe",
        vocab_size=args.vocab_size,
        # Matching Llama's own settings, so a checkpoint trained with
        # llama2.c's train.py tokenizes the way it expects.
        self_test_sample_size=0,
        input_format="text",
        character_coverage=1.0,
        num_threads=8,
        split_digits=True,
        allow_whitespace_only_pieces=True,
        byte_fallback=True,
        unk_id=0, bos_id=1, eos_id=2, pad_id=-1,
        normalization_rule_name="identity",
        input_sentence_size=args.input_sentence_size,
        shuffle_input_sentence=True,
        train_extremely_large_corpus=False,
    )
    n = export_bin(str(args.prefix), f"{args.prefix}.bin")
    print(f"{args.prefix}.model and {args.prefix}.bin: {n} tokens")


if __name__ == "__main__":
    main()
