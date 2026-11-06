#!/usr/bin/env python3
"""Run the card's `r` under C33 Linux in wremu, booted the device's way (MBR
flash -> Grifo -> init.app -> linux.app, through linux/ask-app.py), and
check what Rust's standard library did: arguments, the environment, a file
round trip, four threads over a channel, and a clock; and that panics
unwind: caught, destructors run, across a thread join.  The FAT fixture
cannot make folders, so the shell puts r in /mnt/sd/bin first."""

from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent


def main():
    program = ROOT / "rust/stdhello/stdhello.bin"
    unwind = ROOT / "rust/unwindtest/unwindtest.bin"
    for p in (program, unwind):
        if not p.exists():
            raise SystemExit(f"missing: {p} (rust/build-linux.sh)")
    out = ROOT / "rust/stdhello/ask"
    result = subprocess.run(
        [sys.executable, str(ROOT / "linux/ask-app.py"),
         "--file", f"stdhello.bin={program}", "--file", f"unwind.bin={unwind}",
         "--file", f"sdprobe.bin={ROOT / 'rust/sdprobe/sdprobe.bin'}",
         "--file", f"drmhello.bin={ROOT / 'rust/drmhello/drmhello.bin'}",
         "--file", f"r.sh={ROOT / 'rust/card/bin/r'}", "--output", str(out),
         "mkdir -p /mnt/sd/bin && cp /mnt/sd/r.sh /mnt/sd/bin/r && r && cat /mnt/sd/rust.txt"],
        capture_output=True, timeout=600)
    log = (result.stdout + result.stderr).decode(errors="replace").replace("\r", "")
    sums = ", ".join(f"{i}: {500500 * (i + 1)}" for i in range(4))
    expected = [
        'stdhello: 3 args, first Some("/mnt/sd/stdhello.bin")',
        'stdhello: HOME=Some("/root")',
        "stdhello: file round trip: written by Rust on the C33",
        f"stdhello: thread sums {{{sums}}}",
        'unwindtest: caught "bottom reached", drops 0x1f',
        'unwindtest: thread panicked with &str "thread 7"',
        "unwindtest: second panic caught true, drops 0x11f",
        "unwindtest: done",
        "drmhello: 240x208 D1, drawn in ",
    ]
    missing = [line for line in expected if line not in log]
    if (missing or "panicked at" in log or "PASS (rust.txt)" not in log
            or not re.search(r"stdhello: done in [\d.]+m?s", log)):
        print(log, file=sys.stderr)
        raise SystemExit("stdhello failed; missing: " + ", ".join(missing or ["done"]))
    print("rust passed under C33 Linux: std (args, env, files, threads, channel, "
          "time) and unwinding panics")
    drm_test()


def read_pgm(path):
    parts = path.read_bytes().split(maxsplit=4)
    return int(parts[1]), int(parts[2]), parts[4]


def drm_test():
    """drmhello under openvt, as the device runs it; the run stops while it
    holds the panel, so the emulator's screen.pgm is its picture."""
    program = ROOT / "rust/drmhello/drmhello.bin"
    if not program.exists():
        raise SystemExit(f"missing: {program} (rust/build-linux.sh drmhello)")
    out = ROOT / "rust/drmhello/ask"
    screen = out / "screen.pgm"
    screen.unlink(missing_ok=True)
    result = subprocess.run(
        [sys.executable, str(ROOT / "linux/ask-app.py"), "--cycles", "1500000000",
         "--file", f"drmhello.bin={program}", "--output", str(out),
         "openvt -c 2 -s -w -- sh -c '/mnt/sd/drmhello.bin 600 > /tmp/d.txt 2>&1' & "
         "sleep 3; cat /tmp/d.txt; sleep 600"],
        capture_output=True, timeout=600)
    log = (result.stdout + result.stderr).decode(errors="replace").replace("\r", "")
    drawn = re.search(r"drmhello: 240x208 D1, drawn in ([\d.]+)ms", log)
    if not drawn or not screen.exists():
        print(log, file=sys.stderr)
        raise SystemExit("drmhello did not draw")
    w, h, px = read_pgm(screen)
    black = lambda x, y: px[y * w + x] < 128
    centre = w // 2
    checks = {
        "border": black(2, 2) and black(w - 3, h - 3) and not black(10, 10),
        "filled circle": black(centre - 60, 120),
        "ring's inside": not black(centre - 60 + 25, 120),
        "filled square": black(centre - 80 + 6, 181),
        "outlined square": not black(centre - 60 + 6, 181) and black(centre - 60, 181),
        "triangle base": black(centre + 60, 155),
        "text": sum(black(x, y) for y in range(18, 56) for x in range(20, 220)) > 400,
    }
    failed = [k for k, ok in checks.items() if not ok]
    if failed:
        raise SystemExit("drmhello's picture is wrong: " + ", ".join(failed))
    print(f"drmhello passed: the panel shows its picture, drawn in {drawn.group(1)} ms")


if __name__ == "__main__":
    main()
