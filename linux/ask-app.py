#!/usr/bin/env python3
"""Boot Linux through MBR -> Grifo -> init.app -> linux.app in wremu, type
commands on the serial shell, power off, and print what the shell printed:

    ask-app.py 'uname -a; cat /proc/meminfo' [-- extra wremu arguments]

--file NAME=PATH puts another file on the card, as /mnt/sd/NAME, an 8.3
name; --args gives the kernel command line, as init.ini's linux line does.
"""
import argparse
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile


root = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument("commands")
parser.add_argument("--kernel", type=Path, default=root / "linux/artifacts/linux.app")
parser.add_argument("--rootfs", type=Path, default=root / "linux/artifacts/linux.img")
parser.add_argument("--cycles", type=int, default=4_000_000_000,
                    help="wremu's cycle budget (-n)")
parser.add_argument("--file", action="append", default=[], metavar="NAME=PATH",
                    help="another file for the card, by its 8.3 name")
parser.add_argument("--args", default="",
                    help="the kernel command line, from the init.ini line")
parser.add_argument("--output", type=Path, default=root / "linux/artifacts/ask-app",
                    help="directory for the serial log")
parser.add_argument("wremu", nargs=argparse.REMAINDER,
                    help="extra wremu arguments, after --")
args = parser.parse_args()
extra = args.wremu[1:] if args.wremu[:1] == ["--"] else args.wremu

spec = importlib.util.spec_from_file_location("app", root / "linux/app-test.py")
app = importlib.util.module_from_spec(spec)
spec.loader.exec_module(app)
fat = app.load_fat_helper(root)
files = {name: app.require(path).read_bytes() for name, path in {
    "kernel.elf": root / "samo-lib/grifo/grifo.elf",
    "init.app": root / "samo-lib/grifo/applications/init/init.app",
    "linux.app": args.kernel, "linux.img": args.rootfs,
    "linux.ico": root / "linux/artifacts/linux.ico",
}.items()}
files["init.ini"] = f"linux.ico : linux.app {args.args}\n".encode() * 2
for item in args.file:
    name, _, path = item.partition("=")
    files[name] = app.require(Path(path)).read_bytes()
args.output.mkdir(parents=True, exist_ok=True)
log = args.output.resolve() / "serial.log"
with tempfile.TemporaryDirectory(prefix="wr-ask-app-") as temporary:
    out = Path(temporary)
    card, flash, uart = (out / name for name in ("card.img", "flash.rom", "uart.in"))
    fat.make_image(card, files, 64)
    subprocess.run([sys.executable, str(root / "samo-lib/mbr/make-flash.py"), str(flash)],
                   check=True, stdout=subprocess.DEVNULL)
    uart.write_text(f"{args.commands}\necho ASK-DONE\npoweroff -f\n")
    with log.open("w") as output:
        # The tap opens the Linux icon in Grifo's menu, as signal-test.py's.
        subprocess.run([str(app.require(root / "emulator/wremu")), "-n", str(args.cycles),
                        "-T", "40,36,100000000", "--uart-input", str(uart),
                        "--uart-start", "850000000", "--uart-gap", "200000",
                        "-c", str(card), "-e", str(flash), *extra], cwd=out,
                       stdout=output, stderr=subprocess.STDOUT,
                       env={**os.environ, "WREMU_HOLD_MS": "33"})
text = log.read_text(errors="replace").replace("\r", "")
print(text[-20000:])
if "ASK-DONE" not in text:
    raise SystemExit(f"the shell never finished the commands; see {log}")
