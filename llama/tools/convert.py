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

**Nothing in the output is floating point.** The device has no FPU, so a
scale that arrives as an fp32 would be multiplied by a call into libgcc --
which measured 80% of the whole forward pass before this. Every scale is
written as an int16 mantissa with one shared power-of-two exponent per
tensor, so the device applies it with `mlt.w` and a shift.

Rounding matches `torch.round`: numpy and torch both round halves to even,
so a checkpoint quantized here matches one quantized by `export.py`.
"""

import argparse
import struct
import sys

import numpy as np

MAGIC = b"WRL2"
VERSION = 2
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


def encode_scales(values, what="scales", allow_underflow=0.0):
    """Float array -> (int16 mantissas, shared exponent) with v = m * 2^-e.

    One exponent for the whole array rather than one per element, so the
    device applies a scale with a multiply and a single shift it already
    knows.  The cost is precision on the smallest entries: an array whose
    largest and smallest magnitudes differ by 2^k leaves the smallest with
    15 - k significant bits.

    Every weight and norm array in these checkpoints spreads by at most
    13x, which leaves the smallest entry eleven bits -- but nothing checks
    that for a checkpoint nobody has tried yet, and a scale that quietly
    rounds to zero deletes a whole output row without any symptom but worse
    text.  So it is checked, and `allow_underflow` is the fraction of
    entries a caller is willing to lose.
    """
    v = np.asarray(values, dtype=np.float64)
    vmax = np.abs(v).max()
    if vmax == 0:
        return np.zeros(v.shape, np.int16), 0
    # The largest e with vmax * 2^e still inside an int16.
    e = int(np.floor(np.log2(32767.0 / vmax)))
    m = np.clip(np.round(v * (2.0**e)), -32767, 32767).astype(np.int16)

    nonzero = v != 0
    if nonzero.any():
        lost = np.abs(m[nonzero]) < 8
        fraction = lost.sum() / nonzero.sum()
        if fraction > allow_underflow:
            raise SystemExit(
                f"{what}: {lost.sum()} of {nonzero.sum()} entries lose all "
                f"but three bits under one shared exponent (spread "
                f"{np.abs(v[nonzero]).max() / np.abs(v[nonzero]).min():.0f}x). "
                "A per-row exponent would be needed for this checkpoint."
            )
    return m, e


def write_scaled(f, values, what="scales", allow_underflow=0.0):
    """int16 mantissas followed by the int32 shared exponent."""
    m, e = encode_scales(values, what, allow_underflow)
    if m.size % 2:
        raise SystemExit(f"{what}: scaled vectors must have an even length")
    f.write(m.astype("<i2").tobytes())
    f.write(struct.pack("<i", e))
    return m, e


def convert(src, dst):
    config, t = read_legacy(src)
    n_layers = config["n_layers"]

    # The device loads whole rows with word loads, so every row has to start
    # on a four-byte boundary.  All the shapes here already do; refuse
    # rather than emit a file the fast kernel would misread.
    for name in ("tok_embeddings", "wq", "wk", "wv", "wo", "w1", "w2", "w3"):
        n, d = t[name].shape[-1], t[name].shape[-2]
        if n % 4:
            raise SystemExit(
                f"{name} has row length {n}, which is not a multiple of four; "
                "the device kernel loads rows a word at a time"
            )
        # The mantissa array is int16 and the exponent after it is int32, so
        # an odd row count would leave the exponent unaligned.
        if d % 2:
            raise SystemExit(f"{name} has an odd row count {d}")

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

        # The norms multiply activations rather than being summed over, so
        # they keep more precision than the weights do: int16 mantissas
        # against the weights' int8.
        for i in range(n_layers):
            write_scaled(f, t["attention_norm"][i], f"attention_norm.{i}")
        for i in range(n_layers):
            write_scaled(f, t["ffn_norm"][i], f"ffn_norm.{i}")
        write_scaled(f, t["norm"], "norm")

        # The rotation table, interleaved cos/sin, one pair per (position,
        # frequency).  run.c calls powf, cosf and sinf here for every token
        # it generates, and there is no libm on the device -- mini-libc has
        # no math.h at all.  The values depend only on the checkpoint's
        # seq_len and head_size, so they belong in the file.  They are all
        # in [-1, 1], so the shared exponent makes this plain Q14.
        # One sine in the table underflows: sin of a 3e-5 angle, where
        # rounding to zero turns a rotation that is already the identity to
        # eleven decimal places into the identity.
        write_scaled(f, rope_table(config).reshape(-1), "rope",
                     allow_underflow=0.001)

        worst, worst_name = 0.0, ""
        for name, w in tensors:
            q, s, err = quantize_rows(w)
            f.write(q.tobytes())
            write_scaled(f, s, f"{name} row scales")
            if err > worst:
                worst, worst_name = err, name

    weight_bytes = sum(w.size for _, w in tensors)
    scale_bytes = sum(w.shape[0] * 2 + 4 for _, w in tensors)
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
