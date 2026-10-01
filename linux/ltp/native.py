#!/usr/bin/env python3
"""Run the matching native builds as an unprivileged comparison, not a verdict."""
import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import signal
import subprocess
import tempfile
from run import modern_result


ROOT = Path(__file__).resolve().parents[2]
STATUS = {0: "PASS", 1: "FAIL", 2: "UNRESOLVED", 4: "UNSUPPORTED", 5: "UNTESTED", 124: "TIMEOUT"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binaries", type=Path, default=ROOT / "linux/artifacts/ltp-native-all")
    parser.add_argument("--output", type=Path, default=ROOT / "linux/artifacts/ltp-native-all-run")
    parser.add_argument("--jobs", type=int, default=3)
    parser.add_argument("--timeout", type=int, default=30)
    args = parser.parse_args()
    args.binaries = args.binaries.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    manifest = json.loads((args.binaries / "manifest.json").read_text())

    def run(case):
        log = args.output / (case["binary"] + ".log")
        with tempfile.TemporaryDirectory(prefix="wr-ltp-native-") as directory, log.open("w") as output:
            command = [str(args.binaries / manifest["supervisor"]["binary"]), str(args.timeout),
                       str(args.binaries / case["binary"])]
            process = subprocess.Popen(command, cwd=directory, stdin=subprocess.DEVNULL,
                                       stdout=output, stderr=subprocess.STDOUT, start_new_session=True,
                                       env={**os.environ, "KCONFIG_PATH": str(args.binaries / "kconf.txt")})
            try:
                process.wait(timeout=args.timeout + 10)
                status = STATUS.get(process.returncode, "ERROR")
            except subprocess.TimeoutExpired:
                children = Path(f"/proc/{process.pid}/task/{process.pid}/children")
                if children.exists():
                    for child in children.read_text().split():
                        try:
                            os.killpg(int(child), signal.SIGKILL)
                        except ProcessLookupError:
                            pass
                process.kill()
                process.wait()
                status = "HOST_TIMEOUT"
        record = {"name": case["name"], "exit_status": process.returncode, "status": status, "log": str(log)}
        if manifest.get("result_format") == "ltp" and status != "HOST_TIMEOUT":
            record.update(modern_result(process.returncode, log.read_text(errors="replace")))
        return record

    results = []
    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        for result in pool.map(run, manifest["cases"]):
            results.append(result)
            if len(results) % 50 == 0 or len(results) == len(manifest["cases"]):
                print(len(results), dict(Counter(r["status"] for r in results)), flush=True)
                report = {"build": manifest, "uid": os.getuid(), "guest_timeout": args.timeout, "results": results}
                (args.output / "results.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
