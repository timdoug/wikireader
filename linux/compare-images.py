#!/usr/bin/env python3
"""Compare root images on the same kernel and card, one boot each in
wremu through Grifo (ask-app.py): the boot to the console prompt, memory
available at the prompt, 40 execs of /bin/true from the shell, startx to
the first command an xterm runs beside xeyes and twm, and memory available
with that desktop up.

    compare-images.py saved.img artifacts/linux.img [--runs N]

Times are the guest's (/proc/uptime and the kernel log), so they are the
emulated machine's, not the host's.
"""
import argparse
from pathlib import Path
import re
import statistics
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parent.parent

SESSION = """#!/bin/sh
xeyes -geometry 64x48+172+0 &
WR_XTERM_INITIAL_COMMAND=/mnt/sd/xup.sh xterm -fn 5x8 -geometry 32x22+0+0 -T sh &
exec twm -f /usr/share/X11/twm/wikireader.twmrc
"""
XUP = """#!/bin/sh
cut -d' ' -f1 /proc/uptime > /tmp/xup
"""
MEASURE = """#!/bin/sh
up() { cut -d' ' -f1 /proc/uptime; }
echo "M prompt $(dmesg | grep -o 'prompt up [0-9.]*' | grep -o '[0-9.]*$')"
echo "M avail $(grep MemAvailable /proc/meminfo | grep -o '[0-9]*')"
t0=$(up); i=0
while [ $i -lt 40 ]; do /bin/true; i=$((i + 1)); done
echo "M exec40 $t0 $(up)"
rm -f /tmp/xup
t0=$(up)
startx sh /mnt/sd/xsess.sh >/dev/null 2>&1 &
while [ ! -s /tmp/xup ]; do sleep 0.2; done
echo "M startx $t0 $(cat /tmp/xup)"
sleep 2
echo "M xavail $(grep MemAvailable /proc/meminfo | grep -o '[0-9]*')"
"""


def measure(image, output):
    with tempfile.TemporaryDirectory(prefix="wr-compare-") as tmp:
        tmp = Path(tmp)
        for name, text in (("measure.sh", MEASURE), ("xsess.sh", SESSION), ("xup.sh", XUP)):
            (tmp / name).write_text(text)
        command = [sys.executable, str(root / "linux/ask-app.py"),
                   "--rootfs", str(image), "--cycles", "6000000000",
                   "--output", str(output)]
        for name in ("measure.sh", "xsess.sh", "xup.sh"):
            command += ["--file", f"{name}={tmp / name}"]
        command.append("chmod +x /mnt/sd/xup.sh 2>/dev/null; sh /mnt/sd/measure.sh")
        subprocess.run(command, capture_output=True, check=True)
    log = (output / "serial.log").read_text(errors="replace").replace("\r", "")
    found = dict((m[0], m[1].split()) for m in re.findall(r"^M (\w+) (.*)$", log, re.M))
    for key in ("prompt", "avail", "exec40", "startx", "xavail"):
        if key not in found:
            raise SystemExit(f"{image}: no {key} measurement; see {output / 'serial.log'}")
    return {
        "prompt s": float(found["prompt"][0]),
        "available at prompt KB": int(found["avail"][0]),
        "exec ms": (float(found["exec40"][1]) - float(found["exec40"][0])) * 1000 / 40,
        "startx to xterm s": float(found["startx"][1]) - float(found["startx"][0]),
        "available with X KB": int(found["xavail"][0]),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("images", nargs="+", type=Path)
    parser.add_argument("--runs", type=int, default=1)
    args = parser.parse_args()
    results = {}
    for image in args.images:
        runs = [measure(image, root / "linux/artifacts/compare" / f"{image.stem}-{n}")
                for n in range(args.runs)]
        results[image.name] = {k: statistics.median(r[k] for r in runs) for k in runs[0]}
    keys = list(next(iter(results.values())))
    print(f"{'':26}" + "".join(f"{name:>18}" for name in results))
    for key in keys:
        print(f"{key:26}" + "".join(f"{r[key]:>18.2f}" if isinstance(r[key], float)
                                   else f"{r[key]:>18}" for r in results.values()))


if __name__ == "__main__":
    main()
