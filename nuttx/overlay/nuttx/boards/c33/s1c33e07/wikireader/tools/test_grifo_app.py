#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Boot NuttX the way the launcher does, and check how it stops.

The wikireader:app configuration is loaded off a card by Grifo, the resident
kernel, exactly like zim.app and doom.app.  That makes two things worth
testing that the direct-ELF and FLASH-boot tests cannot see:

  * the icon menu starts NuttX at all, which needs the relinked image to stay
    clear of the 256 KiB Grifo occupies and its ELF sections to be ones
    Grifo's loader will place;
  * NSH's "poweroff" and "reboot" reach Grifo's System_exit with the right
    argument, which needs its trap table back before the "int 1", and that the
    device then does what was asked.

Grifo prints the exit code it was handed, and then either toggles P63 to drop
the power rail or arms the watchdog for a reset.  wremu models both, so the
checks are on what the machine did and not only on what was asked for: after
"poweroff" the run ends, and after "reboot" the launcher comes back up.

Nothing is written outside the output directory; the card image is built from
scratch each run.
"""

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_terminal import read_pgm, write_png            # noqa: E402

# The first icon's centre, from samo-lib/grifo/applications/init: 64x64 icons
# with icon 0 at x 8-71, y 4-67.
ICON0 = (40, 36)

# samo-lib/include/grifo.h
EXIT_POWER_OFF = 1
EXIT_REBOOT = 2


def load_fat_helper(wikireader):
    """The pure-Python FAT32 fixture builder the emulator's tools already use."""
    helper = wikireader / "emulator/tools/mem_dma_bench/run.py"
    spec = importlib.util.spec_from_file_location("wr_fat_fixture", helper)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def build_card(fat, path, wikireader, app):
    """A card holding the launcher, this build, and a second entry.

    init.app chains a lone entry straight through without drawing anything, so
    the menu only exists with two.  The ZIM reader is the natural companion
    when the checkout has one built; failing that a second NuttX entry still
    produces a menu to start from.
    """

    files = {
        "init.app": (wikireader /
                     "samo-lib/grifo/applications/init/init.app").read_bytes(),
        "nuttx.app": app.read_bytes(),
        "nuttx.ico": (wikireader / "nuttx/nuttx.ico").read_bytes(),
    }
    entries = ["nuttx.ico : nuttx.app started-from-init"]

    zim_app, zim_icon = wikireader / "zim/zim.app", wikireader / "zim/zim.ico"
    if zim_app.exists() and zim_icon.exists():
        files["zim.app"] = zim_app.read_bytes()
        files["zim.ico"] = zim_icon.read_bytes()
        entries.append("zim.ico : zim.app started-from-init")
    else:
        entries.append("nuttx.ico : nuttx.app started-from-init")

    files["init.ini"] = ("\n".join(entries) + "\n").encode()
    fat.make_image(path, files)
    for name, data in files.items():
        if fat.read_file(path, name) != data:
            raise SystemExit(f"Card image did not read back {name}")


def run(command, out, name):
    with (out / name).open("w") as log:
        subprocess.run(command, cwd=out, stdout=log, stderr=subprocess.STDOUT,
                       check=True, timeout=1800,
                       env={**os.environ, "WREMU_HOLD_MS": "33"})
    text = (out / name).read_bytes().decode(errors="replace").replace("\r", "")
    if re.search(r"Panic:|PANIC|Assertion|ELF32_load error", text):
        raise SystemExit(f"Guest failure: see {out / name}")
    return re.sub(r"\x1b\[[0-?]*[ -/]*[@-~]", "", text)


def session(emulator, card, kernel, out, tag, commands, limit):
    """Start NuttX from the menu, type commands, and return the transcript."""

    source = out / f"{tag}-input.txt"
    source.write_text(commands)
    command = [str(emulator), "-n", str(limit),
               "-T", f"{ICON0[0]},{ICON0[1]},100000000",
               "--uart-input", str(source),
               "--uart-start", "480000000", "--uart-gap", "200000",
               "-R", "-c", str(card), str(kernel)]
    (out / f"{tag}-command.json").write_text(json.dumps(command, indent=2) + "\n")
    text = run(command, out, f"{tag}.log")
    (out / f"{tag}-console.txt").write_text(text)
    write_png(out / f"{tag}.png", read_pgm(out / "screen.pgm"))

    if "init choosing" not in text:
        raise SystemExit(f"Launcher menu did not appear: see {out / tag}.log")
    if "WIKIREADER_TERMINAL_READY" not in text or "NuttShell" not in text:
        raise SystemExit(f"Tapping the icon did not start NuttX: "
                         f"see {out / tag}-console.txt")
    return text


def check_exit_code(text, code, out, tag):
    """Grifo was handed `code`, which is the argument surviving the trap."""

    if not re.search(rf"^application returned: {code}$", text, re.M):
        got = re.findall(r"^application returned: (-?\d+)$", text, re.M)
        raise SystemExit(f"Expected exit code {code} from {tag}, got "
                         f"{got or 'none'}: see {out / tag}-console.txt")


def boots(text):
    """How many times the launcher has run, i.e. how many times we booted."""

    return len(re.findall(r"init starting", text))


def main():
    root = Path(__file__).resolve().parents[5]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wikireader", type=Path,
                        default=Path.home() / "wikireader")
    parser.add_argument("--image", type=Path, default=root / "nuttx",
                        help="the built kernel; stripped into nuttx.app")
    parser.add_argument("--out", type=Path,
                        default=root / "build/wikireader/grifo-app")
    parser.add_argument("--strip", default="c33-epson-elf-strip")
    parser.add_argument("--compile", action="store_true",
                        help="also compile and run a C file on the device")
    args = parser.parse_args()

    wikireader = args.wikireader.resolve()
    emulator = wikireader / "emulator/wremu"
    kernel = wikireader / "samo-lib/grifo/grifo.elf"
    for needed in (emulator, kernel,
                   wikireader / "samo-lib/grifo/applications/init/init.app",
                   wikireader / "nuttx/nuttx.ico"):
        if not needed.exists():
            print(f"SKIP: {needed} is missing; "
                  f"build the emulator and Grifo first")
            return 0

    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)

    app = out / "nuttx.app"
    subprocess.run([args.strip, "-o", str(app), str(args.image)], check=True)
    (out / "nuttx.app.sha256").write_text(
        hashlib.sha256(app.read_bytes()).hexdigest() + "  nuttx.app\n")

    fat = load_fat_helper(wikireader)
    card = out / "card.img"
    build_card(fat, card, wikireader, app)

    commands = "uname -a\nfree\n"
    if args.compile:
        commands += "tcc -run /tmp/hello.c\n"
    limit = 2_600_000_000 if args.compile else 1_400_000_000

    text = session(emulator, card, kernel, out, "poweroff",
                   commands + "poweroff\n", limit)
    if "wikireader" not in text:
        raise SystemExit("uname did not report the board")
    if args.compile and "Hello from native C33 C!" not in text:
        raise SystemExit("The compiler did not run on the device")
    check_exit_code(text, EXIT_POWER_OFF, out, "poweroff")

    # Grifo drives P63 until the supply outside the chip drops, and wremu
    # stops there.  Reaching the cycle limit instead would mean the rail was
    # never dropped, however good the exit code looked.
    if "stop reason: powered off" not in text:
        raise SystemExit(f"poweroff did not drop the power rail: "
                         f"see {out / 'poweroff-console.txt'}")
    if boots(text) != 1:
        raise SystemExit(f"The device came back after poweroff: "
                         f"see {out / 'poweroff-console.txt'}")

    heap = re.search(r"Umem:\s+(\d+)", text)
    if heap:
        print(f"heap: {int(heap.group(1)):,} bytes")

    text = session(emulator, card, kernel, out, "reboot",
                   "reboot\n", 1_600_000_000)
    check_exit_code(text, EXIT_REBOOT, out, "reboot")

    # ...and here the watchdog it armed has to actually reset the machine,
    # which has to come all the way back up to the launcher.
    if "watchdog reset" not in text:
        raise SystemExit(f"reboot did not reach a watchdog reset: "
                         f"see {out / 'reboot-console.txt'}")
    if boots(text) != 2:
        raise SystemExit(f"The launcher did not come back after reboot "
                         f"({boots(text)} boots): "
                         f"see {out / 'reboot-console.txt'}")

    print(f"PASS: launcher starts NuttX; poweroff drops the rail and stays "
          f"down; reboot resets and comes back up; {out / 'reboot.png'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
