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
            "init.ini": (b"linux.ico : linux.app started-from-init\n"
                         b"linux.ico : linux.app started-from-init\n"),
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
    return 0


if __name__ == "__main__":
    sys.exit(main())
