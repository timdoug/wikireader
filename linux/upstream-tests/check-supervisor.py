#!/usr/bin/env python3
"""Check supervisor status propagation and cleanup on native Linux."""
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    with tempfile.TemporaryDirectory(prefix="wr-supervisor-check-") as directory:
        work = Path(directory)
        binary = work / "supervisor"
        subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-Werror",
                        Path(__file__).with_name("supervise.c"), "-o", binary], check=True)
        checks = [(["/bin/true"], 0), (["/bin/false"], 1),
                  (["/no-such-executable"], 127), (["/bin/sh", "-c", "kill -TERM $$"], 143),
                  (["/bin/sleep", "30"], 124)]
        for command, expected in checks:
            result = subprocess.run([binary, "1", *command], timeout=5)
            if result.returncode != expected:
                raise SystemExit(f"{command}: got {result.returncode}, expected {expected}")
        pidfile = work / "pid"
        command = ["/bin/sh", "-c", f"setsid sleep 30 & echo $! > {pidfile}"]
        subprocess.run([binary, "2", *command], timeout=5, check=True)
        pid = int(pidfile.read_text())
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            pass
        else:
            raise SystemExit("Descendant escaped supervisor cleanup")
    print("Supervisor passed: success, failure, exec error, signal, timeout, escaped-group cleanup")


if __name__ == "__main__":
    main()
