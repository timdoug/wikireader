#!/usr/bin/env python3
"""Check the host build, which is the same code the device runs.

The forward pass is fixed point, so it does not reproduce llama2.c's fp32
output token for token, and demanding that it should would be the wrong
test: under greedy decoding one differing logit picks a different token and
from there the two runs write different stories for reasons that have
nothing to do with arithmetic.

So accuracy is measured teacher-forced -- feed a fixed token sequence and
compare the top-1 prediction at every position against an independent fp32
reference in numpy.  That isolates the arithmetic.  A golden string then
locks the generated text against regressions.
"""

import subprocess
import sys
import urllib.request
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
CACHE = ROOT / "build" / "testdata"
BASE = "https://huggingface.co/karpathy/tinyllamas/resolve/main/stories260K"

sys.path.insert(0, str(ROOT / "tools"))

# What the integer forward pass generates unprompted, greedily.  It follows
# llama2.c's fp32 output for its first 42 tokens and then takes a different
# but equally sensible turn.
GOLDEN = (
    "Once upon a time, there was a little girl named Lily. She loved to "
    "play outside in the park. One day, she saw a big, red ball. She "
    'wanted to play with it, but her mom said, "No, it\'s'
)

# Long enough to exercise a growing KV cache and every layer many times.
TEACHER = (
    "Once upon a time, there was a little girl named Lily. She loved to "
    "play outside in the park. One day, she saw a big, red ball."
)

PROMPT = "The dog ran"


def fetch(name):
    CACHE.mkdir(parents=True, exist_ok=True)
    path = CACHE / name
    if not path.exists():
        print(f"fetching {name}")
        urllib.request.urlretrieve(f"{BASE}/{name}", path)
    return path


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

    def run(*args):
        out = subprocess.run(
            [str(binary), str(model), "-z", str(vocab), "-q", *args],
            capture_output=True, text=True, check=True,
        )
        return out.stdout.rstrip("\n")

    failures = 0

    # 1. Arithmetic, teacher-forced against fp32 in numpy.
    from convert import read_legacy
    from reference import Reference

    rows = [
        tuple(map(int, line.split()))
        for line in run("-teacher", TEACHER).split("\n")
        if line.strip()
    ]
    cfg, tensors = read_legacy(source)
    ref = Reference(cfg, tensors)
    agree = 0
    for pos, (token, got) in enumerate(rows):
        if int(np.argmax(ref.forward(token, pos))) == got:
            agree += 1
    rate = 100.0 * agree / len(rows)
    if agree != len(rows):
        print(f"FAIL: top-1 agreement with fp32 is {agree}/{len(rows)} "
              f"({rate:.1f}%), expected every position")
        failures += 1
    else:
        print(f"ok: top-1 matches fp32 at all {len(rows)} teacher-forced "
              "positions")

    # 2. Generated text, against the recorded output.
    got = run("-n", "64")
    if got != GOLDEN:
        print("FAIL: generated text changed")
        print(f"  expected: {GOLDEN!r}")
        print(f"  got:      {got!r}")
        failures += 1
    else:
        print("ok: generated text matches the golden sample")

    # 3. The prompt has to come back out before generation starts.
    got = run("-n", "24", "-i", PROMPT)
    if not got.startswith(PROMPT):
        print(f"FAIL: prompt did not round-trip; got {got!r}")
        failures += 1
    else:
        print(f"ok: prompt round-trips ({got[:48]!r}...)")

    # 4. Sampling: reproducible for a seed, different across seeds, and
    # not looping the way greedy does on a model this small.
    a = run("-n", "36", "-t", "0.7", "-s", "7", "-i", "lily saw a cat")
    b = run("-n", "36", "-t", "0.7", "-s", "7", "-i", "lily saw a cat")
    c = run("-n", "36", "-t", "0.7", "-s", "8", "-i", "lily saw a cat")
    if a != b:
        print("FAIL: same seed gave different text")
        failures += 1
    elif a == c:
        print("FAIL: different seeds gave identical text")
        failures += 1
    else:
        print(f"ok: sampling is seeded and varies ({a[14:52]!r}...)")

    # The temperature is parsed by hand -- mini-libc has no strtod, so the
    # device cannot use one and the host must not either, or a card and a
    # terminal disagree about what "0.80" means. Equivalent spellings have
    # to give identical text for the same seed.
    for x, y in (("1", "1.0"), ("0.5", "0.50"), ("0.8", "0.800")):
        if run("-n", "20", "-t", x, "-s", "3", "-i", "a cat") != \
           run("-n", "20", "-t", y, "-s", "3", "-i", "a cat"):
            print(f"FAIL: temperature {x!r} and {y!r} parsed differently")
            failures += 1
            break
    else:
        print("ok: equivalent temperature spellings parse the same")

    # 5. A truncated weight file must be refused, not read as garbage
    # weights: that failure mode generates plausible nonsense and reports
    # nothing at all.
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
