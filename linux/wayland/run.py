#!/usr/bin/env python3
"""Run the Wayland session through Grifo: cage, foot and wlkeyboard.

startw brings up cage on the panel with foot above the soft keyboard; taps
on wlkeyboard type "touch /w" and Enter into foot's shell, which must make
the file.  The panel is read back from the driver's scanout in IVRAM.
"""
import argparse
import importlib.util
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
# The DRM driver's converted image: IVRAM, 32 bytes a line, 208 lines.
SCANOUT = 0x80000
# The cycle the taps start from: the session is up by about 3.9e9.
TAPS_FROM = 5000000000


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "linux/artifacts/wayland-run")
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
    }.items()}
    files["init.ini"] = b"linux.ico : linux.app loglevel=7 wr.blank=0\n"
    files["wrun.sh"] = f"""#!/bin/sh
startw >/mnt/sd/startw.log 2>&1 &
n=0
while :; do
    if pidof foot >/dev/null && pidof wlkeyboard >/dev/null; then break; fi
    n=$((n + 1))
    [ "$n" -lt 120 ] || {{ echo WAYLAND FAIL: no session; cat /tmp/cage.log; exit 1; }}
    sleep 1
done
echo "WAYLAND READY at $(cut -d' ' -f1 /proc/uptime) s"
n=0
while :; do
    if [ -e /w ]; then break; fi
    n=$((n + 1))
    [ "$n" -lt 600 ] || {{ echo WAYLAND KEYBOARD FAIL: nothing typed; cat /tmp/cage.log; exit 1; }}
    sleep 1
done
rm /w
echo WAYLAND KEYBOARD PASS
sleep 2
dd if=/dev/mem of=/mnt/sd/screen.d1 bs=32 skip={SCANOUT // 32} count=208 2>/dev/null
grep -v 'XKB\\|^$' /tmp/cage.log > /mnt/sd/cage.log
sync
poweroff
""".encode()
    fat.make_image(out / "card.img", files, 64)
    (out / "flash.rom").unlink(missing_ok=True)
    subprocess.run([sys.executable, str(ROOT / "samo-lib/mbr/make-flash.py"),
                    str(out / "flash.rom")], check=True, stdout=subprocess.DEVNULL)
    (out / "uart.in").write_text("sh /mnt/sd/wrun.sh\n")
    # "touch /w" and Enter on wlkeyboard, laid out as the console's: keys
    # 24 pixels wide in rows 22 high from row 120.  The script waits for
    # the file however late the taps land.
    keys = {"t": (108, 131), "o": (204, 131), "u": (156, 131), "c": (84, 175),
            "h": (132, 153), " ": (84, 197), "w": (36, 131), "123": (36, 197),
            "/": (36, 153), "ABC": (36, 197), "\n": (204, 197)}
    taps = []
    for n, key in enumerate(["t", "o", "u", "c", "h", " ", "123", "/", "ABC", "w", "\n"]):
        x, y = keys[key]
        taps += ["-T", f"{x},{y},{TAPS_FROM + n * 60000000}"]
    with (out / "boot.log").open("w") as log:
        subprocess.run([str(ROOT / "emulator/wremu"), "-n", "20000000000", *taps,
                        "-e", str(out / "flash.rom"), "-c", str(out / "card.img"),
                        "--uart-input", str(out / "uart.in"),
                        "--uart-start", "600000000", "--uart-gap", "200000"],
                       cwd=out, stdout=log, stderr=subprocess.STDOUT,
                       check=True, timeout=1200, env=os.environ)
    for name in ("screen.d1", "cage.log", "startw.log"):
        data = fat.read_file(out / "card.img", name)
        if data is not None:
            (out / name).write_bytes(data)
    text = (out / "boot.log").read_text(errors="replace").replace("\r", "")
    if "WAYLAND KEYBOARD PASS" not in text or re.search(
            r"Kernel panic|C33 exception|stack overflow|WAYLAND .*FAIL", text):
        raise SystemExit(f"Wayland regression failed; see {out / 'boot.log'}")
    screen = (out / "screen.d1").read_bytes()
    assert len(screen) == 32 * 208
    # The keyboard's top edge: a black line across the panel at row 120.
    assert screen[120 * 32:120 * 32 + 30] == b"\xff" * 30, "no keyboard on the panel"
    pgm = bytes(0 if screen[y * 32 + x // 8] & (0x80 >> (x % 8)) else 255
                for y in range(208) for x in range(240))
    (out / "screen.pgm").write_bytes(b"P5 240 208 255\n" + pgm)
    ready = re.search(r"WAYLAND READY at ([0-9.]+) s", text)
    print(f"Wayland passed: cage, foot and wlkeyboard up at {ready.group(1)} s;"
          " wlkeyboard typed \"touch /w\" into foot's shell")
    print(f"Screenshot: {out / 'screen.pgm'}")


if __name__ == "__main__":
    main()
