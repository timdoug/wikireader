#!/usr/bin/env python3
"""Run the Rust applications the way the device does: MBR flash -> Grifo
-> init.app's menu -> a tap on the icon -> the application -> the menu.

hello prints, draws, and returns at the next tap: one run stops while it
waits with its picture on the panel, for screen.pgm, and one taps again.
abitest exchanges every argument shape with C, prints its verdict and
returns at once.
"""

import importlib.util
import re
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
ICON0 = (40, 36)
ICON1 = (112, 36)
TAP = 100_000_000
# Instructions, not MCLK: well after the application is waiting.
TAP2 = TAP + 150_000_000

PRIMES = sum(all(n % d for d in range(2, n) if d * d <= n) for n in range(2, 200))


def fnv1a(data):
    h = 0x811C9DC5
    for b in data:
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h


def require(path):
    if not path.exists():
        raise SystemExit(f"missing: {path}")
    return path


def load_fat_helper():
    helper = ROOT / "emulator/tools/mem_dma_bench/run.py"
    spec = importlib.util.spec_from_file_location("wr_fat_fixture", helper)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def read_pgm(path):
    data = path.read_bytes()
    parts = data.split(maxsplit=4)
    assert parts[0] == b"P5", parts[0]
    width, height = int(parts[1]), int(parts[2])
    pixels = parts[4]
    return width, height, pixels


def main():
    emulator = require(ROOT / "emulator/wremu")
    rust = ROOT / "rust"
    fat = load_fat_helper()
    with tempfile.TemporaryDirectory(prefix="wr-rust-") as temporary:
        out = Path(temporary)
        files = {
            "kernel.elf": require(ROOT / "samo-lib/grifo/grifo.elf").read_bytes(),
            "init.app": require(ROOT / "samo-lib/grifo/applications/init/init.app").read_bytes(),
            # hello in the menu's first slot, abitest in the second.
            "init.ini": b"hello.ico : hello.app\nabitest.ico : abitest.app\n",
        }
        for name in ("hello", "abitest"):
            icon = out / f"{name}.ico"
            subprocess.run([sys.executable,
                            str(ROOT / "samo-lib/grifo/scripts/xpm2icon"),
                            f"--icon={icon}", str(rust / name / f"{name}.xpm")],
                           check=True)
            files[f"{name}.app"] = require(rust / name / f"{name}.app").read_bytes()
            files[f"{name}.ico"] = icon.read_bytes()
        card = out / "card.img"
        flash = out / "flash.rom"
        fat.make_image(card, files, 64)
        subprocess.run([sys.executable, str(ROOT / "samo-lib/mbr/make-flash.py"),
                        str(flash)], check=True, stdout=subprocess.DEVNULL)

        def run(cycles, *taps):
            command = [str(emulator), "-n", str(cycles)]
            for x, y, at in taps:
                command += ["-T", f"{x},{y},{at}"]
            result = subprocess.run(
                command + ["-c", str(card), "-e", str(flash)],
                cwd=out, capture_output=True, timeout=300)
            return (result.stdout + result.stderr).decode(errors="replace").replace("\r", "")

        def returned(log, name):
            if "application returned: 0" not in log or log.count("init starting") < 2:
                print(log, file=sys.stderr)
                raise SystemExit(f"rust {name} did not return to the launcher")

        expected = [
            "rust hello: argc=1",
            f"rust hello: primes={PRIMES} fnv1a={fnv1a(b'WikiReader'):#010x}",
            "rust hello: waiting for a tap",
        ]
        log = run(TAP2, (*ICON0, TAP))
        missing = [m for m in expected if m not in log]
        if missing or "rust panic" in log:
            print(log, file=sys.stderr)
            raise SystemExit("rust hello failed; missing: " + ", ".join(missing))
        screen = rust / "hello/screen.pgm"
        screen.write_bytes((out / "screen.pgm").read_bytes())
        width, height, pixels = read_pgm(screen)
        black = lambda x, y: pixels[y * width + x] < 128
        triangle = [black(56 + u, 64 + v) == (u & v == 0)
                    for v in range(0, 128, 3) for u in range(0, 128, 3)]
        if not all(triangle):
            raise SystemExit(f"triangle wrong at {triangle.count(False)} of "
                             f"{len(triangle)} sampled pixels")
        returned(run(TAP2 + 100_000_000, (*ICON0, TAP), (120, 150, TAP2)), "hello")
        print(f"rust hello passed: menu -> hello.app -> tap -> menu; screen in {screen}")

        # abitest checks itself and returns at once.
        log = run(TAP2, (ICON1[0], ICON1[1], TAP))
        verdict = re.search(r"abi: (\d+) checks, (\d+) failures: ABI (PASS|FAIL)", log)
        failures = [line for line in log.splitlines() if line.startswith("abi: ")]
        if not verdict or verdict.group(3) != "PASS" or "rust panic" in log:
            print("\n".join(failures) or log, file=sys.stderr)
            raise SystemExit("rust abitest failed")
        returned(log, "abitest")
        print(f"rust abitest passed: {verdict.group(1)} Rust/C calls agree")


if __name__ == "__main__":
    main()
