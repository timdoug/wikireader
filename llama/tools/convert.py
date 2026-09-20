#!/usr/bin/env python3
"""Quantize a llama2.c checkpoint to int8 for the WikiReader.

llama2.c ships its TinyStories models as fp32.  The device cannot afford
that: there is no FPU on this part, `__mulsf3` is 226 instructions, and a
float multiply-accumulate costs around 250 cycles against about twelve for
an int8 one.  Every weight has to be an integer before any of this is worth
running.

**Scales are per output row, not per fixed-size group.**  Upstream's Q8_0
uses groups of 64, and `runq.c` walks them with `for (j = 0; j <= n - GS;
j += GS)`, which assumes the row length `n` is a multiple of the group
size.  stories260K has `hidden_dim = 172`, so on the `w2` matmul that loop
covers 128 of 172 weights and drops the other 44 without saying so -- the
model generates "Once upon upon upon upon" and nothing in the tool chain
reports a problem.  A scale per row divides every tensor exactly whatever
its shape, and costs one float multiply per output element instead of one
per group.  On this model it is also no less accurate, because `dim` is 64
and the two schemes then agree exactly.

Rounding matches `torch.round`: numpy and torch both round halves to even,
so a checkpoint quantized here matches one quantized by `export.py`.
"""

import argparse
import struct
import sys

import numpy as np

MAGIC = b"WRL1"
VERSION = 1
HEADER_BYTES = 64


def read_legacy(path):
    """Return (config, dict of fp32 tensors) from a version 1 checkpoint."""
    with open(path, "rb") as f:
        raw = f.read()

    dim, hidden_dim, n_layers, n_heads, n_kv_heads, vocab_size, seq_len = struct.unpack(
        "7i", raw[:28]
    )
    # Legacy files signal an unshared classifier with a negative vocab size.
    shared_classifier = vocab_size > 0
    vocab_size = abs(vocab_size)
    head_size = dim // n_heads

    data = np.frombuffer(raw, dtype=np.float32, offset=28)
    pos = 0

    def take(*shape):
        nonlocal pos
        count = int(np.prod(shape))
        out = data[pos : pos + count].reshape(shape)
        pos += count
        return out

    kv_dim = n_kv_heads * head_size
    t = {}
    # Shapes are (out_features, in_features): the matmul reads w[i * n + j]
    # with i over outputs, so a row is one output's worth of weights and is
    # contiguous.  That is also the unit a scale covers.
    t["tok_embeddings"] = take(vocab_size, dim)
    t["attention_norm"] = take(n_layers, dim)
    t["wq"] = take(n_layers, dim, dim)
    t["wk"] = take(n_layers, kv_dim, dim)
    t["wv"] = take(n_layers, kv_dim, dim)
    t["wo"] = take(n_layers, dim, dim)
    t["ffn_norm"] = take(n_layers, dim)
    t["w1"] = take(n_layers, hidden_dim, dim)
    t["w2"] = take(n_layers, dim, hidden_dim)
    t["w3"] = take(n_layers, hidden_dim, dim)
    t["norm"] = take(dim)
    # What used to be the precomputed RoPE tables; run.c recomputes them and
    # so do we, from a table built at startup.
    pos += seq_len * head_size // 2
    pos += seq_len * head_size // 2
    if not shared_classifier:
        t["output"] = take(vocab_size, dim)

    if pos != data.size:
        raise SystemExit(
            f"{path}: consumed {pos} floats of {data.size}; the header does "
            "not describe this file"
        )

    config = dict(
        dim=dim,
        hidden_dim=hidden_dim,
        n_layers=n_layers,
        n_heads=n_heads,
        n_kv_heads=n_kv_heads,
        kv_dim=kv_dim,
        vocab_size=vocab_size,
        seq_len=seq_len,
        shared_classifier=shared_classifier,
    )
    return config, t


def rope_table(config):
    """Interleaved cos/sin for every (position, frequency) pair.

    run.c rotates dimension `i` of each head by `pos / 10000^(head_dim /
    head_size)` where `head_dim = i % head_size`.  That depends on `i` only
    through `i % head_size`, and only on even `i`, so there are head_size/2
    distinct frequencies and seq_len * head_size/2 distinct rotations in the
    whole model.  Laying them out as [pos][freq][cos, sin] means the device
    reads the pair it needs with two consecutive loads.
    """
    head_size = config["dim"] // config["n_heads"]
    pos = np.arange(config["seq_len"], dtype=np.float64)[:, None]
    head_dim = np.arange(0, head_size, 2, dtype=np.float64)[None, :]
    freq = 1.0 / np.power(10000.0, head_dim / head_size)
    val = pos * freq
    return np.stack([np.cos(val), np.sin(val)], axis=-1).astype(np.float32)


def quantize_rows(w):
    """Symmetric int8 in [-127, 127] with one fp32 scale per row."""
    f = w.astype(np.float32)
    wmax = np.abs(f).max(axis=-1)
    # An all-zero row would divide by zero.  Its weights quantize to zero
    # under any scale, so any nonzero one reconstructs it exactly.
    scale = np.where(wmax == 0, 1.0, wmax / 127.0).astype(np.float32)
    q = np.round(f / scale[..., None]).astype(np.int8)
    err = np.abs(q.astype(np.float32) * scale[..., None] - f).max()
    return q, scale, float(err)


def convert(src, dst):
    config, t = read_legacy(src)
    n_layers = config["n_layers"]

    # The device loads whole rows with word loads, so every row has to start
    # on a four-byte boundary.  All the shapes here already do; refuse
    # rather than emit a file the fast kernel would misread.
    for name in ("tok_embeddings", "wq", "wk", "wv", "wo", "w1", "w2", "w3"):
        n = t[name].shape[-1]
        if n % 4:
            raise SystemExit(
                f"{name} has row length {n}, which is not a multiple of four; "
                "the device kernel loads rows a word at a time"
            )

    # Weights in the order the forward pass reads them, a layer at a time,
    # so that a model too big for RAM can later be streamed from the card in
    # the order it is used rather than seeked around.
    tensors = [("tok_embeddings", t["tok_embeddings"])]
    for i in range(n_layers):
        for name in ("wq", "wk", "wv", "wo", "w1", "w2", "w3"):
            tensors.append((f"{name}.{i}", t[name][i]))
    if not config["shared_classifier"]:
        tensors.append(("output", t["output"]))

    with open(dst, "wb") as f:
        f.write(MAGIC)
        f.write(struct.pack("<I", VERSION))
        f.write(
            struct.pack(
                "<7i",
                config["dim"],
                config["hidden_dim"],
                n_layers,
                config["n_heads"],
                config["n_kv_heads"],
                config["vocab_size"],
                config["seq_len"],
            )
        )
        f.write(struct.pack("<I", int(config["shared_classifier"])))
        pad = HEADER_BYTES - f.tell()
        if pad < 0:
            raise SystemExit("header overflowed its 64 bytes")
        f.write(b"\0" * pad)

        # The norms stay fp32.  There are only (2 * n_layers + 1) * dim of
        # them and they scale activations rather than being summed over, so
        # quantizing them would cost accuracy and save nothing measurable.
        for i in range(n_layers):
            f.write(t["attention_norm"][i].astype("<f4").tobytes())
        for i in range(n_layers):
            f.write(t["ffn_norm"][i].astype("<f4").tobytes())
        f.write(t["norm"].astype("<f4").tobytes())

        # The rotation table, interleaved cos/sin, one pair per (position,
        # frequency).  run.c calls powf, cosf and sinf here for every token
        # it generates, and there is no libm on the device -- mini-libc has
        # no math.h at all.  The values depend only on the checkpoint's
        # seq_len and head_size, so they belong in the file.
        f.write(rope_table(config).astype("<f4").tobytes())

        worst, worst_name = 0.0, ""
        for name, w in tensors:
            q, s, err = quantize_rows(w)
            f.write(q.tobytes())
            f.write(s.astype("<f4").tobytes())
            if err > worst:
                worst, worst_name = err, name

    weight_bytes = sum(w.size for _, w in tensors)
    scale_bytes = sum(w.shape[0] * 4 for _, w in tensors)
    print(
        f"{dst}: dim {config['dim']}, hidden {config['hidden_dim']}, "
        f"{n_layers} layers, {config['n_heads']} heads "
        f"({config['n_kv_heads']} kv), vocab {config['vocab_size']}, "
        f"seq {config['seq_len']}"
    )
    print(
        f"  {weight_bytes} weight bytes + {scale_bytes} scale bytes "
        f"+ {HEADER_BYTES} header"
    )
    print(f"  worst row error {worst:.6f} in {worst_name}")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("source", help="llama2.c version 1 (fp32) checkpoint")
    ap.add_argument("dest", help="int8 output for the device")
    args = ap.parse_args()
    convert(args.source, args.dest)
    return 0


if __name__ == "__main__":
    sys.exit(main())
