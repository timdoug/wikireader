#!/usr/bin/env python3
"""Run BusyBox's upstream source lint on its complete Linux build tree."""
import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--guest", type=Path, default=Path("/home/timdoug.guest/wr-linux"))
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    source = args.guest / "ltp-isolated/buildroot-build/build/busybox-1.38.0"
    script = source / "testsuite/all_sourcecode.tests"
    prefix = args.guest / "ltp-isolated/toolchain/install/bin/c33-linux-uclibc-"
    env = {**os.environ, "srcdir": str(source / "testsuite"), "CROSS_COMPILE": str(prefix),
           "ECHO": "/usr/bin/echo", "VERBOSE": "1", "LC_ALL": "C"}
    with tempfile.TemporaryDirectory(prefix="wr-busybox-source-") as directory:
        shutil.copyfile(source / "testsuite/testing.sh", Path(directory) / "testing.sh")
        with (args.output / "source.log").open("w") as log:
            result = subprocess.run(["/bin/sh", script], cwd=directory, env=env,
                                    stdout=log, stderr=subprocess.STDOUT)
    text = (args.output / "source.log").read_text()
    assertions = Counter(re.findall(r"^(PASS|FAIL|SKIPPED):", text, re.M))
    report = {"scope": "Linux host source checks; no C33 execution", "exit_status": result.returncode,
              "source": str(source), "assertions": dict(assertions),
              "script_sha256": hashlib.sha256(script.read_bytes()).hexdigest(),
              "environment": {key: env[key] for key in ("srcdir", "CROSS_COMPILE", "ECHO", "VERBOSE", "LC_ALL")}}
    (args.output / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    print(report["assertions"])
    return result.returncode if sum(assertions.values()) == 5 else 125


if __name__ == "__main__":
    raise SystemExit(main())
