#!/usr/bin/env python3
"""Exercise real xterm PTYs and the default xeyes desktop through Grifo."""
import argparse
import importlib.util
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "linux/artifacts/x11-run")
    parser.add_argument("--kernel", type=Path, default=ROOT / "linux/artifacts/linux.app")
    parser.add_argument("--rootfs", type=Path, default=ROOT / "linux/artifacts/linux.img")
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    spec = importlib.util.spec_from_file_location("wr_app", ROOT / "linux/app-test.py")
    app = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(app)
    fat = app.load_fat_helper(ROOT)
    files = {name: app.require(path).read_bytes() for name, path in {
        "kernel.elf": ROOT / "samo-lib/grifo/grifo.elf",
        "init.app": ROOT / "samo-lib/grifo/applications/init/init.app",
        "linux.app": args.kernel,
        "linux.img": args.rootfs,
        "linux.ico": ROOT / "linux/artifacts/linux.ico",
        "xpty.bin": ROOT / "linux/artifacts/x11-tests/pty-check",
        "xwin.bin": ROOT / "linux/artifacts/x11-tests/window-check",
    }.items()}
    files["init.ini"] = b"linux.ico : linux.app loglevel=7 wr.blank=0\n"
    files["xtest.sh"] = b"""#!/bin/sh
xeyes -geometry 64x48+172+0 &
WR_XTERM_INITIAL_COMMAND=/mnt/sd/xpty.bin xterm -fn 5x8 -geometry 32x22+0+0 -T sh &
exec twm -f /usr/share/X11/twm/wikireader.twmrc
"""
    files["xrun.sh"] = b"""#!/bin/sh
set -e
startx sh /mnt/sd/xtest.sh >/mnt/sd/x11.log 2>&1 &
session=$!
n=0
while :; do
    if grep -q 'XTERM PTY PASS' /mnt/sd/xterm.txt 2>/dev/null; then
        if DISPLAY=:0 /mnt/sd/xwin.bin --shell >/mnt/sd/xwin.txt 2>&1; then break; fi
    fi
    n=$((n + 1))
    [ "$n" -lt 30 ] || { cat /mnt/sd/xterm.txt /mnt/sd/x11.log /tmp/Xfbdev.log; exit 1; }
    sleep 1
done
echo XTERM INTERACTIVE PTY PASS
cat /mnt/sd/xterm.txt /mnt/sd/xwin.txt
killall twm
wait "$session"
sleep 1
if pidof xterm xeyes Xfbdev >/dev/null; then
    echo XTERM CLEANUP FAIL
    ps
    exit 1
fi
echo XTERM CLEANUP PASS
startx >/mnt/sd/xdemo.log 2>&1 &
n=0
while :; do
    if DISPLAY=:0 /mnt/sd/xwin.bin >/mnt/sd/xdemo.txt 2>&1; then break; fi
    n=$((n + 1))
    [ "$n" -lt 30 ] || { cat /mnt/sd/xdemo.log /tmp/Xfbdev.log; exit 1; }
    sleep 1
done
sleep 1
cat /dev/fb0 >/mnt/sd/x11.fb
cat /mnt/sd/xdemo.txt
echo XTERM DEMO PASS
"""
    fat.make_image(out / "card.img", files, 64)
    (out / "flash.rom").unlink(missing_ok=True)
    subprocess.run([sys.executable, str(ROOT / "samo-lib/mbr/make-flash.py"),
                    str(out / "flash.rom")], check=True, stdout=subprocess.DEVNULL)
    (out / "uart.in").write_text("sh /mnt/sd/xrun.sh\n")
    with (out / "boot.log").open("w") as log:
        subprocess.run([str(ROOT / "emulator/wremu"), "-n", "3500000000",
                        "-e", str(out / "flash.rom"), "-c", str(out / "card.img"),
                        "--uart-input", str(out / "uart.in"),
                        "--uart-start", "600000000", "--uart-gap", "200000"],
                       cwd=out, stdout=log, stderr=subprocess.STDOUT,
                       check=True, timeout=300, env=os.environ)
    for name in ("xterm.txt", "xshell.txt", "xwin.txt", "x11.log", "xdemo.txt", "xdemo.log", "x11.fb"):
        data = fat.read_file(out / "card.img", name)
        if data is not None:
            (out / name).write_bytes(data)
    text = (out / "boot.log").read_text(errors="replace").replace("\r", "")
    for marker in ("XTERM INTERACTIVE PTY PASS", "XTERM SHELL PASS", "XTERM CLEANUP PASS", "XTERM DEMO PASS"):
        if marker not in text:
            raise SystemExit(f"Missing {marker}; see {out / 'boot.log'}")
    if re.search(r"Kernel panic|C33 exception|XTERM .*FAIL", text):
        raise SystemExit(f"X11 regression failed; see {out / 'boot.log'}")
    assert (out / "xterm.txt").read_text().endswith("XTERM PTY PASS\n")
    assert (out / "x11.fb").stat().st_size == 32 * 208
    print("Xterm passed: controlling PTY, foreground session, TERM/WINDOWID, geometry, live shell after initial command")
    print("X11 passed: 5x8 font; xterm and xeyes side by side; session cleanup; uname/cpuinfo desktop")
    print(f"Screenshot: {out / 'screen.pgm'}")


if __name__ == "__main__":
    main()
