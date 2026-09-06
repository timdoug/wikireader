#!/usr/bin/env python3
"""Fit wremu's WREMU_MODEL parameters to a device bench.txt.

    tools/fit_model.py DEVICE-bench.txt CARD.dmg [--rounds N] [--jobs J]

Runs the ZIM_BENCH build of the reader on CARD.dmg (which must contain it as
zim.app, first on the launcher menu) under candidate parameter sets, parses
the `bench` lines, and minimises the squared log ratio to the device's
micro-benchmarks by coordinate descent, one parameter at a time, with the
candidates of a parameter evaluated in parallel.  The CPU/SDRAM parameters
are fitted against the memory tests, then the card parameters against the
card tests.  Prints the best set as a WREMU_MODEL string and the per-test
ratios; bake the values into src/model.c when satisfied.
"""
import argparse
import math
import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
WREMU = os.path.join(HERE, "..", "wremu")
FLASH = os.path.join(HERE, "..", "..", "samo-lib", "mbr", "flash.rom")

MEMORY_TESTS = ["cpu-loop", "fetch-1k", "cpu-loop-a0", "fetch-a0", "cpu-loop-ivram", "fetch-ivram",
                "cpu-loop-dstram", "fetch-dstram", "read-words", "read-bytes", "write-words",
                "write-bytes", "pair-same-row", "pair-row-change", "pair-two-banks",
                "pair-write-read", "copy-bytes-512k", "copy-batch8-512k", "memcpy-512k"]
CARD_TESTS = ["card-256k", "card-4k-x64"]

MEMORY_PARAMS = {
    "branch_taken": [3, 4, 5, 6],
    "branch_taken_iram": [3, 4, 5],
    "iqb_first": [0, 1, 2, 3, 4],
    "iqb_word_gap": [0, 1, 2, 3],
    "dq_extra": [0, 1, 2, 3, 4],
    "wr_ticks": [0, 1, 2, 3],
    "iram_fetch_wait": [0, 1, 2, 3],
    "wr_rd_turn": [0, 1, 2, 3, 4],
}
CARD_PARAMS = {
    "dma_extra": [0, 5, 10, 15, 20, 25, 30],
    "sd_read_latency": [0, 40000, 60000, 80000, 100000, 120000],
}


def parse_bench(text):
    out = {}
    for line in text.splitlines():
        m = re.match(r"bench (\S+)\s+(\d+) ops\s+([\d.]+) ms\s+([\d.]+) cyc/op", line)
        if m:
            out[m.group(1)] = float(m.group(4))
    return out


def run(card, params):
    env = dict(os.environ)
    env["WREMU_MODEL"] = ",".join(f"{k}={v}" for k, v in params.items())
    cmd = [WREMU, "-R", "-e", FLASH, "-c", card, "-T", "40,36,100000000", "-n", "260000000"]
    p = subprocess.run(cmd, env=env, capture_output=True, text=True, errors="replace")
    return parse_bench(p.stdout)


def error(measured, device, tests):
    err = 0.0
    for t in tests:
        if t in measured and t in device and measured[t] and device[t]:
            err += math.log(measured[t] / device[t]) ** 2
        else:
            err += 4.0
    return err


def fit(card, device, params, tests, rounds, jobs):
    current = {k: v[0] for k, v in params.items()}
    best_err = error(run(card, current), device, tests)
    print(f"start {current} err {best_err:.4f}", flush=True)
    for r in range(rounds):
        improved = False
        for name, values in params.items():
            trials = []
            for v in values:
                if v == current[name]:
                    continue
                cand = dict(current)
                cand[name] = v
                trials.append(cand)
            with ThreadPoolExecutor(max_workers=jobs) as pool:
                results = list(pool.map(lambda c: (c, error(run(card, c), device, tests)), trials))
            for cand, err in results:
                if err < best_err - 1e-9:
                    best_err, current, improved = err, cand, True
            print(f"round {r + 1} {name}: {current[name]} err {best_err:.4f}", flush=True)
        if not improved:
            break
    return current, best_err


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("device")
    ap.add_argument("card")
    ap.add_argument("--rounds", type=int, default=3)
    ap.add_argument("--jobs", type=int, default=6)
    args = ap.parse_args()

    with open(args.device) as f:
        device = parse_bench(f.read())   # the last block wins
    missing = [t for t in MEMORY_TESTS + CARD_TESTS if t not in device]
    if missing:
        print(f"note: device file lacks {missing}; fitting without them", file=sys.stderr)
    memory_tests = [t for t in MEMORY_TESTS if t in device]
    if not memory_tests or not all(t in device for t in CARD_TESTS):
        sys.exit("device file has no usable benchmark lines")

    mem, mem_err = fit(args.card, device, MEMORY_PARAMS, memory_tests, args.rounds, args.jobs)
    card_params = dict(CARD_PARAMS)
    fixed = dict(mem)
    # Fit the card with the memory parameters held.
    def fit_card():
        current = dict(fixed)
        current.update({k: v[0] for k, v in card_params.items()})
        best = error(run(args.card, current), device, CARD_TESTS)
        for _ in range(args.rounds):
            improved = False
            for name, values in card_params.items():
                trials = []
                for v in values:
                    if v == current[name]:
                        continue
                    cand = dict(current)
                    cand[name] = v
                    trials.append(cand)
                with ThreadPoolExecutor(max_workers=args.jobs) as pool:
                    results = list(pool.map(lambda c: (c, error(run(args.card, c), device, CARD_TESTS)), trials))
                for cand, err in results:
                    if err < best - 1e-9:
                        best, current, improved = err, cand, True
                print(f"card {name}: {current[name]} err {best:.4f}", flush=True)
            if not improved:
                break
        return current, best
    final, card_err = fit_card()

    measured = run(args.card, final)
    print("\nWREMU_MODEL=" + ",".join(f"{k}={v}" for k, v in final.items()))
    print(f"memory error {mem_err:.4f}, card error {card_err:.4f}")
    print(f"{'test':18} {'device':>10} {'model':>10} {'model/device':>13}")
    for t in MEMORY_TESTS + CARD_TESTS:
        if t in measured and t in device:
            print(f"{t:18} {device[t]:10.1f} {measured[t]:10.1f} {measured[t] / device[t]:13.2f}")


if __name__ == "__main__":
    main()
