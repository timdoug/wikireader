#!/usr/bin/env python3
"""Boot Linux from Grifo's tiled launcher and require a reboot back to it."""

import importlib.util
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


ICON0 = (40, 36)


def load_fat_helper(root):
    helper = root / "emulator/tools/mem_dma_bench/run.py"
    spec = importlib.util.spec_from_file_location("wr_fat_fixture", helper)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def require(path):
    if not path.exists():
        raise SystemExit(f"Required launcher test input is missing: {path}")
    return path


LAUNCHER_ARGS = b"earlycon=s1c33,mmio,0x300b00 loglevel=7 wr.blank=5"


def suspend_run(root, emulator, files, make_flash, fat):
    """A single-entry card auto-boots Linux; leave it alone until it sleeps."""
    with tempfile.TemporaryDirectory(prefix="wr-linux-suspend-") as temporary:
        out = Path(temporary)
        card = out / "card.img"
        flash = out / "flash.rom"
        log = out / "boot.log"
        card_files = dict(files)
        card_files["init.ini"] = b"linux.ico : linux.app wr.blank=2 wr.suspend=6\n"
        fat.make_image(card, card_files, 64)
        subprocess.run([sys.executable, str(make_flash), str(flash)],
                       check=True, stdout=subprocess.DEVNULL)
        command = [
            str(emulator), "-n", "3000000000",
            # Nothing else runs, so the guest idles into suspend long before
            # this tap; wremu advances an idle machine to its next scripted
            # event, which is the touch that has to wake it.
            "-T", f"{ICON0[0]},{ICON0[1]},2500000000",
            "-R", "-c", str(card), "-e", str(flash),
        ]
        with log.open("w") as output:
            subprocess.run(command, cwd=out, stdout=output,
                           stderr=subprocess.STDOUT, check=True, timeout=600,
                           env={**os.environ, "WREMU_HOLD_MS": "33"})
        text = log.read_bytes().decode(errors="replace").replace("\r", "")

    expected = [
        "C33 power: suspending until touch",
        "PM: suspend entry (s2idle)",
        "PM: suspend exit",
        "C33 power: resumed",
        "C33 display: woken by touch",
    ]
    missing = [marker for marker in expected if marker not in text]
    if missing or re.search(r"Kernel panic|suspend REFUSED", text):
        print(text, file=sys.stderr)
        raise SystemExit("Linux suspend regression failed; missing: " +
                         ", ".join(missing or ["a clean suspend"]))
    print("Suspend passed: idle -> s2idle -> touch -> resume")


def main():
    root = Path(__file__).resolve().parent.parent
    emulator = require(root / "emulator/wremu")
    grifo = require(root / "samo-lib/grifo/grifo.elf")
    launcher = require(root / "samo-lib/grifo/applications/init/init.app")
    app = require(root / "linux/artifacts/linux.app")
    icon = require(root / "linux/artifacts/linux.ico")
    make_flash = require(root / "samo-lib/mbr/make-flash.py")
    fat = load_fat_helper(root)

    with tempfile.TemporaryDirectory(prefix="wr-linux-app-") as temporary:
        out = Path(temporary)
        card = out / "card.img"
        flash = out / "flash.rom"
        uart_input = out / "uart.in"
        log = out / "boot.log"
        files = {
            "kernel.elf": grifo.read_bytes(),
            "init.app": launcher.read_bytes(),
            "linux.app": app.read_bytes(),
            "linux.ico": icon.read_bytes(),
            # A second entry makes init.app draw the menu instead of chaining.
            # The arguments are the kernel command line: the launcher is the
            # only thing on this machine that can supply one, and it comes
            # from a line on the card that needs no rebuild to change.
            "init.ini": (b"linux.ico : linux.app " + LAUNCHER_ARGS + b"\n"
                         b"linux.ico : linux.app " + LAUNCHER_ARGS + b"\n"),
        }

        fat.make_image(card, files, 64)
        for name, expected in files.items():
            if fat.read_file(card, name) != expected:
                raise SystemExit(f"Launcher fixture did not read back {name}")
        subprocess.run([sys.executable, str(make_flash), str(flash)],
                       check=True, stdout=subprocess.DEVNULL)
        uart_input.write_text("echo C33 LINUX APP PASS\nreboot -f\n")

        command = [
            str(emulator), "-n", "2000000000",
            "-T", f"{ICON0[0]},{ICON0[1]},100000000",
            "--uart-input", str(uart_input),
            "--uart-start", "850000000", "--uart-gap", "200000",
            "-R", "-c", str(card), "-e", str(flash),
        ]
        with log.open("w") as output:
            subprocess.run(command, cwd=out, stdout=output,
                           stderr=subprocess.STDOUT, check=True, timeout=300,
                           env={**os.environ, "WREMU_HOLD_MS": "33"})
        text = log.read_bytes().decode(errors="replace").replace("\r", "")

        expected = [
            "init choosing",
            "C33 Linux: entry",
            # The launcher's arguments reached the kernel, and asking for an
            # early console on that line produced one before the driver probed.
            "Kernel command line: console=ttyC0,115200 " +
            LAUNCHER_ARGS.decode(),
            "bootconsole [s1c33] enabled",
            # wr.blank=5 on that same line reaches the console frontend, so an
            # idle panel powers down without a VT to blank it.
            "C33 display: blanked while idle",
            "C33 boot: Grifo application (incoming TTBR 00000400)",
            "*** HARDWARE PASS: BusyBox 1.38 is PID 1 on native C33 Linux ***",
            "C33 LINUX APP PASS",
            "application returned: 2",
            "watchdog reset",
        ]
        missing = [marker for marker in expected if marker not in text]
        if missing or len(re.findall(r"^init starting$", text, re.M)) < 2 or \
           re.search(r"Kernel panic|Panic:|ELF32_load error", text):
            print(text, file=sys.stderr)
            raise SystemExit("Launcher Linux regression failed; missing: " +
                             ", ".join(missing or ["second launcher boot"]))

    print("Launcher Linux passed: Grifo menu -> linux.app -> BusyBox -> "
          "reboot -> Grifo menu")
    suspend_run(root, emulator, files, make_flash, fat)
    return 0


if __name__ == "__main__":
    sys.exit(main())
