#!/usr/bin/env python3
"""Run the Rust hello application the way the device does: MBR flash ->
Grifo -> init.app's menu -> a tap on the icon -> hello.app, which prints,
draws, and returns to the launcher at the next tap.

Two runs: one stopped while the application waits with its picture on the
panel, for screen.pgm, and one with the second tap, for the return to the
launcher.
"""

import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
ICON0 = (40, 36)
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
    app_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "rust/hello"
    app = require(app_dir / "hello.app")
    fat = load_fat_helper()
    with tempfile.TemporaryDirectory(prefix="wr-rust-") as temporary:
        out = Path(temporary)
        icon = out / "hello.ico"
        subprocess.run([sys.executable,
                        str(ROOT / "samo-lib/grifo/scripts/xpm2icon"),
                        f"--icon={icon}", str(app_dir / "hello.xpm")],
                       check=True)
        files = {
            "kernel.elf": require(ROOT / "samo-lib/grifo/grifo.elf").read_bytes(),
            "init.app": require(ROOT / "samo-lib/grifo/applications/init/init.app").read_bytes(),
            "hello.app": app.read_bytes(),
            "hello.ico": icon.read_bytes(),
            # Two entries, so init.app draws its menu instead of chaining.
            "init.ini": b"hello.ico : hello.app\n" * 2,
        }
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
        (app_dir / "screen.pgm").write_bytes((out / "screen.pgm").read_bytes())
        width, height, pixels = read_pgm(out / "screen.pgm")
        # The triangle: the apex column at x = 120 is black from y = 64 down,
        # and the Sierpinski hole below its middle row is white.
        black = lambda x, y: pixels[y * width + x] < 128
        triangle = [black(56 + u, 64 + v) == (u & v == 0)
                    for v in range(0, 128, 3) for u in range(0, 128, 3)]
        if not all(triangle):
            raise SystemExit(f"triangle wrong at {triangle.count(False)} of "
                             f"{len(triangle)} sampled pixels")

        log = run(TAP2 + 100_000_000, (*ICON0, TAP), (120, 150, TAP2))
        if "application returned: 0" not in log or log.count("init starting") < 2:
            print(log, file=sys.stderr)
            raise SystemExit("rust hello did not return to the launcher")
    print("rust hello passed: Grifo menu -> hello.app (Rust) -> launcher; "
          f"screen in {app_dir / 'screen.pgm'}")


if __name__ == "__main__":
    main()
