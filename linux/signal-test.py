#!/usr/bin/env python3
"""Run the signal ABI regression through MBR -> Grifo -> Linux."""
import argparse
import importlib.util
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


root = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--kernel", type=Path, default=root / "linux/artifacts/linux.app")
parser.add_argument("--binary", type=Path, default=root / "linux/artifacts/signal-test")
parser.add_argument("--output", type=Path, default=root / "linux/artifacts/signal-test-run")
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
spec = importlib.util.spec_from_file_location("app", root / "linux/app-test.py")
app = importlib.util.module_from_spec(spec)
spec.loader.exec_module(app)
fat = app.load_fat_helper(root)
files = {name: app.require(path).read_bytes() for name, path in {
    "kernel.elf": root / "samo-lib/grifo/grifo.elf",
    "init.app": root / "samo-lib/grifo/applications/init/init.app",
    "linux.app": args.kernel, "linux.img": root / "linux/artifacts/linux.img",
    "linux.ico": root / "linux/artifacts/linux.ico", "sigtest.bin": args.binary,
}.items()}
files["init.ini"] = b"linux.ico : linux.app loglevel=7 earlycon=s1c33,mmio,0x300b00\n" * 2
with tempfile.TemporaryDirectory(prefix="wr-signal-test-") as temporary:
    out = Path(temporary)
    card, flash, uart = (out / name for name in ("card.img", "flash.rom", "uart.in"))
    fat.make_image(card, files, 64)
    subprocess.run([sys.executable, str(root / "samo-lib/mbr/make-flash.py"), str(flash)],
                   check=True, stdout=subprocess.DEVNULL)
    uart.write_text("/mnt/sd/sigtest.bin; reboot -f\n")
    log = args.output.resolve() / "boot.log"
    with log.open("w") as output:
        subprocess.run([str(app.require(root / "emulator/wremu")), "-n", "2500000000",
                        "-T", "40,36,100000000", "--uart-input", str(uart),
                        "--uart-start", "850000000", "--uart-gap", "200000",
                        "-c", str(card), "-e", str(flash)], cwd=out,
                       stdout=output, stderr=subprocess.STDOUT, check=True, timeout=300,
                       env={**os.environ, "WREMU_HOLD_MS": "33"})
text = log.read_text(errors="replace").replace("\r", "")
print("\n".join(re.findall(r"SIGNAL [^\n]+", text)))
if "SIGNAL PASS:" not in text or "SIGNAL FAIL:" in text or \
        "watchdog reset" not in text or "Kernel panic" in text:
    raise SystemExit(f"Signal regression failed; see {log}")
