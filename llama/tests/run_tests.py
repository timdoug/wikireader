#!/usr/bin/env python3
"""Check the host build against llama2.c's own fp32 output.

The quantized model is not expected to agree with fp32 forever -- greedy
decoding turns any logit difference into a different token, after which the
two paths separate -- but stories260K agrees for the whole unprompted
sample, which is a strong signal that the weights, the layout, the RoPE
table and the tokenizer are all right.
"""

import subprocess
import sys
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
CACHE = ROOT / "build" / "testdata"
BASE = "https://huggingface.co/karpathy/tinyllamas/resolve/main/stories260K"

# What upstream `run.c stories260K.bin -z tok512.bin -t 0 -n 64` prints.
EXPECTED = (
    "Once upon a time, there was a little girl named Lily. She loved to "
    "play outside in the park. One day, she saw a big, red ball. She "
    "wanted to play with it, but it was too high.\nLily"
)

# Prompt encoding has to round-trip: the prompt tokens are fed back in, so
# a bad encoder shows up as mangled text before generation even starts.
PROMPT = "The dog ran"


def fetch(name):
    CACHE.mkdir(parents=True, exist_ok=True)
    path = CACHE / name
    if not path.exists():
        print(f"fetching {name}")
        urllib.request.urlretrieve(f"{BASE}/{name}", path)
    return path


def run(binary, model, vocab, *args):
    out = subprocess.run(
        [str(binary), str(model), "-z", str(vocab), "-q", *args],
        capture_output=True, text=True, check=True,
    )
    return out.stdout.rstrip("\n")


def main():
    binary = ROOT / "build" / "wrllama"
    if not binary.exists():
        raise SystemExit(f"{binary} not built; run `make host`")

    source = fetch("stories260K.bin")
    vocab = fetch("tok512.bin")
    model = CACHE / "stories260K.wrl"
    subprocess.run(
        [sys.executable, str(ROOT / "tools" / "convert.py"),
         str(source), str(model)],
        check=True, capture_output=True,
    )

    failures = 0

    got = run(binary, model, vocab, "-n", "64")
    if got != EXPECTED:
        print("FAIL: unprompted sample does not match llama2.c fp32")
        print(f"  expected: {EXPECTED!r}")
        print(f"  got:      {got!r}")
        failures += 1
    else:
        print("ok: unprompted sample matches llama2.c fp32 for 64 tokens")

    got = run(binary, model, vocab, "-n", "24", "-i", PROMPT)
    if not got.startswith(PROMPT):
        print(f"FAIL: prompt did not round-trip; got {got!r}")
        failures += 1
    else:
        print(f"ok: prompt round-trips ({got[:48]!r}...)")

    # A truncated weight file must be refused, not read as garbage weights:
    # that failure mode generates plausible nonsense and reports nothing.
    short = CACHE / "truncated.wrl"
    short.write_bytes(model.read_bytes()[: model.stat().st_size // 2])
    out = subprocess.run(
        [str(binary), str(short), "-z", str(vocab), "-n", "4"],
        capture_output=True, text=True,
    )
    if out.returncode == 0 or "truncated" not in out.stderr:
        print(f"FAIL: truncated model not rejected ({out.stderr.strip()!r})")
        failures += 1
    else:
        print("ok: truncated weight file is rejected")

    print("FAILED" if failures else "all tests passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
