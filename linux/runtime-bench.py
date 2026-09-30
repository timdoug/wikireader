#!/usr/bin/env python3
"""Time real userspace work after MBR -> Grifo -> Linux."""
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
parser.add_argument("--image", type=Path, default=root / "linux/artifacts/linux.img")
parser.add_argument("--kernel", type=Path, default=root / "linux/artifacts/linux.app")
parser.add_argument("--binary", type=Path, default=root / "linux/artifacts/runtime-bench")
parser.add_argument("--argument", default="", help="guest benchmark argument, e.g. --argument=--bench")
parser.add_argument("--profile", type=Path, help="save marked-window PC cycles and boot logs here")
parser.add_argument("--profile-workload", choices=("mutex", "exec"), default="mutex",
                    help="profile the default marked loop or runtime-bench's spawn/exec loop")
args = parser.parse_args()
if args.profile:
    args.profile = args.profile.resolve()
    args.profile.mkdir(parents=True, exist_ok=True)
spec = importlib.util.spec_from_file_location("app", root / "linux/app-test.py")
app = importlib.util.module_from_spec(spec)
spec.loader.exec_module(app)
fat = app.load_fat_helper(root)
files = {name: app.require(path).read_bytes() for name, path in {
    "kernel.elf": root / "samo-lib/grifo/grifo.elf",
    "init.app": root / "samo-lib/grifo/applications/init/init.app",
    "linux.app": args.kernel, "linux.img": args.image,
    "linux.ico": root / "linux/artifacts/linux.ico", "bench.bin": args.binary,
}.items()}
files["init.ini"] = b"linux.ico : linux.app loglevel=7 earlycon=s1c33,mmio,0x300b00\n" * 2
with tempfile.TemporaryDirectory(prefix="wr-runtime-bench-") as temporary:
    out = Path(temporary)
    card, flash, log, uart = (out / name for name in ("card.img", "flash.rom", "boot.log", "uart.in"))
    fat.make_image(card, files, 64)
    subprocess.run([sys.executable, str(root / "samo-lib/mbr/make-flash.py"), str(flash)],
                   check=True, stdout=subprocess.DEVNULL)
    uart.write_text(f"/mnt/sd/bench.bin {args.argument}; reboot -f\n")
    def run(extra):
        with log.open("w") as output:
            subprocess.run([str(app.require(root / "emulator/wremu")), "-n", "4000000000",
                            "-T", "40,36,100000000", "--uart-input", str(uart),
                            "--uart-start", "850000000", "--uart-gap", "200000",
                            "-c", str(card), "-e", str(flash), *extra], cwd=out,
                           stdout=output, stderr=subprocess.STDOUT, check=True, timeout=300,
                           env={**os.environ, "WREMU_HOLD_MS": "33"})
        text = log.read_text(errors="replace")
        if "BENCH PASS" not in text or "watchdog reset" not in text or "Kernel panic" in text:
            print(text, file=sys.stderr)
            raise SystemExit("Runtime benchmark failed")
        return text
    text = run([])
    if args.profile:
        (args.profile / "timing.log").write_text(text)
        marker = "BENCH EXEC PC window" if args.profile_workload == "exec" else "BENCH PC window"
        pattern = re.escape(marker) + r": (0x[0-9a-f]+) (0x[0-9a-f]+)"
        window = re.search(pattern, text)
        if not window:
            raise SystemExit("Benchmark has no profile markers")
        # Recreate the original card: the first boot wrote power/PM files.
        fat.make_image(card, files, 64)
        text = run(["-Y", ",".join(window.groups()), "-F", str(args.profile / "pc.txt")])
        (args.profile / "profile.log").write_text(text)
        second = re.search(pattern, text)
        if not second or second.groups() != window.groups():
            raise SystemExit("Profile run changed the executable's load addresses")
    print("\n".join(re.findall(r"BENCH [^\r\n]+", text)))
