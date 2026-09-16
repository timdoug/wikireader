#!/usr/bin/env python3
"""Run ubench on the emulated device and print what it wrote.

run_benchmarks.py drives the `bench` command, which is a suite of whole
programs; this drives `ubench`, which is the instrument underneath it --
loops with nothing in them but the thing being measured, and the thing the
timing model is fitted against.  Everything about the boot is the same and
comes from there: the same card, the same grifo, the same machine state,
because the device is never in the state a direct ELF boot starts in.

  python3 run_ubench.py --wikireader ../../../../../../.. --app nuttx.app \\
                        --grifo .../grifo.elf

On the hardware: boot NuttX from the launcher, type `ubench` at the prompt,
wait for it to say the card can come out, and read /sd/ubench.txt.  Compare
the two with --compare.
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys

import run_benchmarks

RESULTS = "ubench.txt"


def run_emulator(args):
    emulator = args.wikireader.resolve() / "emulator/wremu"
    if not emulator.exists():
        raise SystemExit(f"no emulator at {emulator}; run make in emulator/")
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    card = out / "card.img"
    fat = run_benchmarks.load_fat_helper(args.wikireader.resolve())
    # The whole boot, not just the application: mask ROM, MBR, kernel.elf,
    # init.app.  Handing the emulator a bare grifo.elf is what
    # run_benchmarks.py still does and the emulator now refuses -- a direct
    # boot leaves the SDRAM controller, the clocks and the serial line in a
    # state the hardware is never in, which is exactly what a timing
    # measurement must not do.
    fat.make_image(card, {"kernel.elf": args.grifo.resolve().read_bytes(),
                          "init.app": args.app.resolve().read_bytes()}, 64)
    flash = out / "flash.rom"
    subprocess.run([sys.executable,
                    str(args.wikireader.resolve() / "samo-lib/mbr/make-flash.py"),
                    str(flash)], check=True, stdout=subprocess.DEVNULL)

    (out / "input.txt").write_text(f"{args.command}\n")
    command = [str(emulator), "-c", str(card), "-e", str(flash),
               "-n", str(args.limit),
               "--uart-input", str(out / "input.txt"),
               # Late enough that the prompt is there to type at: grifo has
               # to bring the card up and load three megabytes off it first.
               "--uart-start", "250000000", "--uart-gap", "200000"]
    with (out / "ubench.log").open("w") as log:
        env = {**os.environ, "WREMU_HOLD_MS": "33"}
        env.setdefault("WREMU_BOARD_REV", "7")   # the board being compared
        subprocess.run(command, cwd=out, stdout=log, stderr=subprocess.STDOUT,
                       check=True, timeout=7200, env=env)

    written = fat.read_file(card, RESULTS)
    if not written:
        console = run_benchmarks.clean(
            (out / "ubench.log").read_bytes().decode(errors="replace"))
        (out / "console.txt").write_text(console)
        raise SystemExit(f"nothing was written to the card; console in "
                         f"{out / 'console.txt'}")
    text = run_benchmarks.clean(written.decode(errors="replace"))
    (out / RESULTS).write_text(text)
    return text


def rows(text):
    """name -> cyc/instr, from either side's file."""
    found = {}
    for line in text.splitlines():
        parts = line.split()
        if len(parts) >= 6 and parts[0] == "UB":
            found[" ".join(parts[1:-5])] = float(parts[-1])
    return found


def main():
    here = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wikireader", type=Path, default=here)
    parser.add_argument("--app", type=Path, required=True)
    parser.add_argument("--grifo", type=Path, required=True)
    parser.add_argument("--out", type=Path, default=Path("/tmp/wr-ubench"))
    parser.add_argument("--limit", type=int, default=400000000000)
    parser.add_argument("--command", default="ubench",
                        help="what to type at the prompt")
    parser.add_argument("--compare", type=Path,
                        help="the device's ubench.txt, for a side-by-side")
    args = parser.parse_args()

    text = run_emulator(args)
    if not args.compare:
        sys.stdout.write(text)
        return 0

    model, device = rows(text), rows(args.compare.read_text())
    print(f'{"loop":18}{"model":>10}{"device":>10}{"ratio":>8}')
    for name, m in model.items():
        d = device.get(name)
        print(f'{name:18}{m:10.2f}' +
              (f'{d:10.2f}{d / m:8.3f}' if d else f'{"-":>10}{"-":>8}'))
    return 0


if __name__ == "__main__":
    sys.exit(main())
