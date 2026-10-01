#!/usr/bin/env python3
"""Run every built case in bounded launcher-path batches and retain results."""
import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binaries", type=Path, default=ROOT / "linux/artifacts/ltp-all")
    parser.add_argument("--output", type=Path, default=ROOT / "linux/artifacts/ltp-sweep")
    parser.add_argument("--rootfs", type=Path, default=ROOT / "linux/artifacts/linux.img")
    parser.add_argument("--kernel", type=Path, default=ROOT / "linux/artifacts/linux.app")
    parser.add_argument("--emulator", type=Path, default=ROOT / "emulator/wremu")
    parser.add_argument("--batch-size", type=int, default=20)
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--timeout", type=int, default=30)
    parser.add_argument("--wall-timeout", type=int, default=600)
    parser.add_argument("--timeouts", type=Path, help="JSON object of per-case guest deadlines")
    args = parser.parse_args()
    args.binaries = args.binaries.resolve()
    args.output = args.output.resolve()
    args.rootfs = args.rootfs.resolve()
    args.kernel = args.kernel.resolve()
    args.emulator = args.emulator.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    manifest = json.loads((args.binaries / "manifest.json").read_text())
    count = len(manifest["cases"])

    def batch(start):
        out = args.output / f"batch-{start:04d}"
        out.mkdir(exist_ok=True)
        command = [sys.executable, str(ROOT / "linux/ltp/run.py"), "--binaries", str(args.binaries),
                   "--output", str(out), "--start", str(start), "--count", str(args.batch_size),
                   "--timeout", str(args.timeout), "--rootfs", str(args.rootfs),
                   "--kernel", str(args.kernel),
                   "--emulator", str(args.emulator),
                   "--wall-timeout", str(args.wall_timeout)]
        if args.timeouts:
            command += ["--timeouts", str(args.timeouts.resolve())]
        with (out / "runner.log").open("w") as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
        result = json.loads((out / "results.json").read_text())
        # Missing cases after a crashed/hung batch receive their own fresh
        # boot so one case cannot hide the results of all following cases.
        missing = [r["name"] for r in result["results"] if r["status"] == "MISSING"]
        for name in missing:
            retry = out / name.replace("/", "-")
            retry.mkdir(exist_ok=True)
            with (retry / "runner.log").open("w") as log:
                retry_command = [sys.executable, str(ROOT / "linux/ltp/run.py"),
                                "--binaries", str(args.binaries), "--output", str(retry),
                                "--case", name, "--timeout", str(args.timeout),
                                "--wall-timeout", str(args.wall_timeout), "--rootfs", str(args.rootfs),
                                "--kernel", str(args.kernel)]
                retry_command += ["--emulator", str(args.emulator)]
                if args.timeouts:
                    retry_command += ["--timeouts", str(args.timeouts.resolve())]
                subprocess.run(retry_command, stdout=log, stderr=subprocess.STDOUT)
            individual = json.loads((retry / "results.json").read_text())
            for r in result["results"]:
                if r["name"] == name:
                    r.update(individual["results"][0], retry=str(retry), retry_complete=individual["complete"])
        print(f"Batch {start}/{count}: {dict(Counter(r['status'] for r in result['results']))}", flush=True)
        # Keep reports, manifests and logs, not dozens of redundant root images.
        for directory in [out] + [out / name.replace("/", "-") for name in missing]:
            (directory / "card.img").unlink(missing_ok=True)
        return {"directory": str(out), "complete": result["complete"], "results": result["results"]}

    reports = []
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for report in pool.map(batch, range(0, count, args.batch_size)):
            reports.append(report)
            totals = Counter(r["status"] for batch in reports for r in batch["results"])
            summary = {"revision": manifest["revision"], "built": count, "build_exclusions": manifest["not_built"],
                       "counts": dict(totals), "complete": len(reports) == (count + args.batch_size - 1) // args.batch_size
                       and all(b["complete"] for b in reports), "batches": reports}
            (args.output / "results.json").write_text(json.dumps(summary, indent=2) + "\n")


if __name__ == "__main__":
    main()
