#!/usr/bin/env python3
"""An fp32 forward pass in numpy, to measure what the integer one costs.

This is llama2.c's `run.c` arithmetic, in float64 where it matters, from
the *original* checkpoint rather than the quantized one.  It exists so the
device's answer can be compared against something, and so that comparison
can be teacher-forced: free-running greedy generation turns a single
differing logit into a different token and then into a different story,
which says nothing about whether the arithmetic is sound.
"""

import numpy as np


def softmax(x):
    e = np.exp(x - x.max())
    return e / e.sum()


class Reference:
    def __init__(self, config, tensors):
        self.c = config
        self.t = tensors
        c = config
        head_size = c["dim"] // c["n_heads"]
        self.head_size = head_size
        self.kv_dim = c["kv_dim"]
        self.kv_mul = c["n_heads"] // c["n_kv_heads"]
        self.key_cache = np.zeros(
            (c["n_layers"], c["seq_len"], c["kv_dim"]), np.float64
        )
        self.value_cache = np.zeros_like(self.key_cache)
        # Same rotation table the device is shipped.
        pos = np.arange(c["seq_len"], dtype=np.float64)[:, None]
        hd = np.arange(0, head_size, 2, dtype=np.float64)[None, :]
        val = pos * (1.0 / np.power(10000.0, hd / head_size))
        self.cos, self.sin = np.cos(val), np.sin(val)

    def rmsnorm(self, x, w):
        ss = (x * x).mean() + 1e-5
        return x / np.sqrt(ss) * w

    def forward(self, token, pos):
        c, t = self.c, self.t
        dim, hs = c["dim"], self.head_size
        x = t["tok_embeddings"][token].astype(np.float64)

        for l in range(c["n_layers"]):
            xb = self.rmsnorm(x, t["attention_norm"][l])
            q = t["wq"][l] @ xb
            k = t["wk"][l] @ xb
            v = t["wv"][l] @ xb

            for vec, n in ((q, dim), (k, self.kv_dim)):
                for i in range(0, n, 2):
                    p = (i % hs) // 2
                    fcr, fci = self.cos[pos, p], self.sin[pos, p]
                    a, b = vec[i], vec[i + 1]
                    vec[i], vec[i + 1] = a * fcr - b * fci, a * fci + b * fcr

            self.key_cache[l, pos] = k
            self.value_cache[l, pos] = v

            xb = np.zeros(dim)
            for h in range(c["n_heads"]):
                qh = q[h * hs : (h + 1) * hs]
                off = (h // self.kv_mul) * hs
                ks = self.key_cache[l, : pos + 1, off : off + hs]
                att = softmax(ks @ qh / np.sqrt(hs))
                vs = self.value_cache[l, : pos + 1, off : off + hs]
                xb[h * hs : (h + 1) * hs] = att @ vs

            x = x + t["wo"][l] @ xb

            xb = self.rmsnorm(x, t["ffn_norm"][l])
            h1 = t["w1"][l] @ xb
            h3 = t["w3"][l] @ xb
            h1 = h1 / (1.0 + np.exp(-h1)) * h3
            x = x + t["w2"][l] @ h1

        x = self.rmsnorm(x, t["norm"])
        w = t.get("output", t["tok_embeddings"])
        return w @ x
