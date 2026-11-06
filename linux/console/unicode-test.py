#!/usr/bin/env python3
"""Check native UTF-8 terminal/shell behavior and actual DRM pixels via Grifo."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", type=Path, default=ROOT / "linux/artifacts/linux.app")
    parser.add_argument("--rootfs", type=Path, default=ROOT / "linux/artifacts/linux.img")
    parser.add_argument("--output", type=Path, default=ROOT / "linux/artifacts/unicode-run")
    parser.add_argument("--ram", type=int, choices=(16, 32), default=32)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    (out / "results.json").unlink(missing_ok=True)
    spec = importlib.util.spec_from_file_location("wr_app", ROOT / "linux/app-test.py")
    app = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(app)
    fat = app.load_fat_helper(ROOT)
    tests = ROOT / "linux/artifacts/unicode-tests"
    paths = {
        "kernel.elf": ROOT / "samo-lib/grifo/grifo.elf",
        "init.app": ROOT / "samo-lib/grifo/applications/init/init.app",
        "linux.app": args.kernel, "linux.img": args.rootfs,
        "linux.ico": ROOT / "linux/artifacts/linux.ico",
        "term.bin": tests / "terminal-test", "pty.bin": tests / "unicode-pty-test",
        "demo.txt": tests / "demo.txt",
    }
    files = {name: app.require(path).read_bytes() for name, path in paths.items()}
    files["init.ini"] = b"linux.ico : linux.app loglevel=7 wr.blank=0\n" * 2
    files["utest.sh"] = b"""#!/bin/sh
set -e
[ "$LANG" = C.UTF-8 ]
/mnt/sd/term.bin
/mnt/sd/pty.bin
cat /mnt/sd/demo.txt > /dev/pts/0
sleep 1
echo UNICODE INTEGRATION PASS
"""
    fat.make_image(out / "card.img", files, 64)
    (out / "flash.rom").unlink(missing_ok=True)
    subprocess.run([sys.executable, str(ROOT / "samo-lib/mbr/make-flash.py"),
                    str(out / "flash.rom")], check=True, stdout=subprocess.DEVNULL)
    (out / "uart.in").write_text("sh /mnt/sd/utest.sh\n")
    with (out / "boot.log").open("w") as log:
        subprocess.run([str(ROOT / "emulator/wremu"), "-n", "3000000000",
                        "-T", "40,36,100000000",
                        "-e", str(out / "flash.rom"), "-c", str(out / "card.img"),
                        "--uart-input", str(out / "uart.in"), "--uart-start", "850000000",
                        "--uart-gap", "200000"], cwd=out, stdout=log,
                       stderr=subprocess.STDOUT, check=True, timeout=240,
                       env={**os.environ, "WREMU_BOARD_REV": "8" if args.ram == 16 else "7"})
    text = (out / "boot.log").read_text(errors="replace").replace("\r", "")
    for marker in ("TERMINAL PASS: 0 failed", "UNICODE PTY PASS:", "UNICODE INTEGRATION PASS\n",
                   f"C33 memory: {args.ram} MiB of SDRAM"):
        if marker not in text:
            raise SystemExit(f"Missing {marker.strip()}; see {out / 'boot.log'}")
    if "Kernel panic" in text or "C33 exception" in text:
        raise SystemExit(f"Guest fault; see {out / 'boot.log'}")
    parts = (out / "screen.pgm").read_bytes().split(b"\n", 3)
    if parts[:3] != [b"P5", b"240 208", b"255"] or len(parts[3]) != 240 * 208:
        raise SystemExit("Unexpected LCD capture")
    expected = (tests / "expected.raw").read_bytes()
    if len(expected) != 120 * 32:
        raise SystemExit("Unexpected reference bitmap")
    differences = sum(parts[3][y * 240 + x] !=
                      (0 if expected[y * 32 + x // 8] & (0x80 >> (x % 8)) else 255)
                      for y in range(120) for x in range(240))
    if differences:
        raise SystemExit(f"LCD differs from Unicode reference at {differences} pixels")
    report = {"ram_mib": args.ram, "terminal": "PASS", "shell_pty": "PASS", "lcd": "PASS",
              "emulator_sha256": hashlib.sha256((ROOT / "emulator/wremu").read_bytes()).hexdigest(),
              "reference_sha256": hashlib.sha256(expected).hexdigest(),
              "sha256": {name: hashlib.sha256(data).hexdigest() for name, data in files.items()}}
    (out / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"Unicode passed ({args.ram} MiB): terminal, interactive Hush editing/completion, exact DRM pixels")
    print(f"Screenshot: {out / 'screen.pgm'}")


if __name__ == "__main__":
    main()
