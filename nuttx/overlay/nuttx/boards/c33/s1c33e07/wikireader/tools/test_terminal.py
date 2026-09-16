#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise the WikiReader LCD NSH terminal using touchscreen packets."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import zlib

import wr_boot


class Keyboard:
    def __init__(self, start):
        self.cycle = start
        self.args = []
        self.symbols = False
        self.taps = 0
        self.letters = self.mapping(("qwertyuiop", "asdfghjkl", " zxcvbnm.?"))
        self.shifted = self.mapping(("!@#$%^&*()", "[]{}<>|~`", " \\\"'=+_,.:"))
        self.numbers = self.mapping(("1234567890", "-/:;()$&@", " \\\"'=+*_?!"))

    @staticmethod
    def mapping(rows):
        return {char: row * 10 + col for row, chars in enumerate(rows)
                for col, char in enumerate(chars) if char != " "}

    def key(self, key):
        x, y = key % 10 * 24 + 12, 120 + key // 10 * 22 + 11
        if key >= 30:
            x = (12, 36, 60, 108, 156, 180, 216)[key - 30]
        self.args += ["-T", f"{x},{y},{self.cycle}"]
        self.cycle += 4_000_000
        self.taps += 1

    def text(self, text):
        special = {" ": 33, "\n": 36, "\b": 19, "\t": 32}
        for char in text:
            if char in special:
                self.key(special[char])
                continue
            want_symbols = char.lower() not in self.letters
            if want_symbols != self.symbols:
                self.key(31)
                self.symbols = want_symbols
            mapping = self.numbers if self.symbols else self.letters
            if char.lower() not in mapping:
                mapping = self.shifted
                self.key(20)
            elif char.isupper():
                self.key(20)
            self.key(mapping[char.lower()])

    def interrupt(self):
        if self.symbols:
            self.key(31)
            self.symbols = False
        self.key(30)
        self.key(self.letters["c"])


def read_pgm(path):
    header, pixels = path.read_bytes().split(b"\n255\n", 1)
    width, height = map(int, header.splitlines()[-1].split())
    if (width, height, len(pixels)) != (240, 208, 240 * 208):
        raise SystemExit("Unexpected emulator screenshot dimensions")
    return pixels


def write_png(path, pixels, scale=3):
    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data +
                struct.pack(">I", zlib.crc32(kind + data)))

    rows = []
    for y in range(208):
        row = b"\0" + b"".join(bytes([v]) * scale
                                for v in pixels[y * 240:(y + 1) * 240])
        rows.extend([row] * scale)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" +
                     chunk(b"IHDR", struct.pack(">IIBBBBB", 240 * scale,
                           208 * scale, 8, 0, 0, 0, 0)) +
                     chunk(b"IDAT", zlib.compress(b"".join(rows))) +
                     chunk(b"IEND", b""))


def run(command, out, name):
    with (out / name).open("w") as log:
        subprocess.run(command, cwd=out, stdout=log, stderr=subprocess.STDOUT,
                       check=True, timeout=300,
                       env={**os.environ, "WREMU_HOLD_MS": "33"})
    text = (out / name).read_bytes().decode(errors="replace").replace("\r", "")
    if re.search(r"PANIC|Assertion|stop reason:|ERROR|command not found|"
                 r"nsh: .*failed|terminal failed", text):
        raise SystemExit(f"Guest failure: see {out / name}")
    if "WIKIREADER_TERMINAL_READY" not in text:
        raise SystemExit(f"Terminal did not start: see {out / name}")
    text = re.sub(r"  \[(?:tap|drag)[^\n]*\]\n", "", text)
    return re.sub(r"\x1b\[[0-?]*[ -/]*[@-~]", "", text)


def main():
    root = Path(__file__).resolve().parents[5]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wikireader", type=Path, default=Path.home() / "wikireader")
    parser.add_argument("--image", type=Path, default=root / "nuttx")
    parser.add_argument("--out", type=Path, default=root / "build/wikireader/lcd")
    parser.add_argument("--flash", type=Path,
                        help="boot this FLASH image instead of a built one")
    parser.add_argument("--card", type=Path,
                        help="with --flash, the card that image boots from")
    args = parser.parse_args()
    if bool(args.flash) != bool(args.card):
        parser.error("--flash and --card must be supplied together")
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    payload = args.image.read_bytes()
    (out / "kernel.elf").write_bytes(payload)
    (out / "kernel.sha256").write_text(hashlib.sha256(payload).hexdigest() +
                                      "  kernel.elf\n")
    emulator = str(args.wikireader.resolve() / "emulator/wremu")
    if args.flash:
        card, flash = args.card.resolve(), args.flash.resolve()
    else:
        # The whole boot, the way the device does it: the image under test
        # goes on a card as init.app, under the grifo the loader chains to.
        card = out / "card.img"
        wr_boot.make_card(card, out / "kernel.elf", args.wikireader)
        flash = wr_boot.make_flash(out, args.wikireader)
    boot = ["-R", *wr_boot.boot_args(card, flash)]
    start = int(wr_boot.UART_START)
    run([emulator, "-n", str(start), *boot], out, "startup.log")
    initial = read_pgm(out / "screen.pgm")
    write_png(out / "startup.png", initial)
    script = Keyboard(start)
    script.text("echo touchok\n")
    script.text('echo "Touch #1!"\n')
    script.text("echo abx\b")
    script.key(34)
    script.text("c")
    script.key(35)
    script.text("\n")
    script.text("unam\t -a\n")
    script.args += ["-G", f"12,131,100,{script.cycle}"]
    script.cycle += 8_000_000
    script.text("echo dragok\n")
    script.text("sleep 30\n")
    script.cycle += 10_000_000
    script.interrupt()
    script.text("echo interruptok\n")
    script.text("sh -c 'sleep 30'\n")
    script.cycle += 10_000_000
    script.interrupt()
    script.text("echo childok\n")
    script.text("echo shouldnotrun")
    script.interrupt()
    script.text("echo cleared\n")
    script.text("exit\n")
    script.cycle += 20_000_000
    script.text("echo restartok\nps\nfree\necho lcd ready\n")
    command = [emulator, "-n", str(script.cycle + 30_000_000), *script.args, *boot]
    (out / "command.json").write_text(json.dumps(command, indent=2) + "\n")
    text = run(command, out, "emulator.log")
    (out / "console.txt").write_text(text)
    for marker in ("touchok", "Touch #1!", "acb", "dragok", "interruptok",
                   "childok", "cleared", "restartok", "lcd ready"):
        if not re.search(r"^" + re.escape(marker) + r"$", text, re.M):
            raise SystemExit(f"Missing shell output {marker!r}: see {out / 'console.txt'}")
    for marker in ("NuttX", "wr_touch", "wr_nx", "Umem"):
        if marker not in text:
            raise SystemExit(f"Missing diagnostic output: {marker}")
    if re.search(r"^shouldnotrun$", text, re.M):
        raise SystemExit("Ctrl-C executed the unfinished command")
    if "nsh> echo touchok" not in text:
        raise SystemExit("Typed characters were not echoed at the shell prompt")
    final = read_pgm(out / "screen.pgm")
    if final[120 * 240:] != initial[120 * 240:]:
        raise SystemExit("Keyboard changed after terminal scrolling or touch release")
    for x in range(0, 240, 24):
        if initial[120 * 240 + x] != 0 or initial[121 * 240 + x + 1] != 255:
            raise SystemExit("Keyboard border/interior pixels are incorrect")
    write_png(out / "screen.png", final)
    serial = out / "serial"
    serial.mkdir(exist_ok=True)
    commands = serial / "commands.txt"
    commands.write_bytes(b"sh -c 'sleep 30'\n\x03echo childok\n"
                         b"echo shouldnotrun\x03echo cleared\n")
    serialtext = run([emulator, "--uart-input", str(commands), "--uart-start",
                      str(start), "--uart-gap", "2000000", "-n",
                      str(start + 220_000_000), *boot], serial, "emulator.log")
    for marker in ("childok", "cleared"):
        if not re.search(r"^" + marker + r"$", serialtext, re.M):
            raise SystemExit(f"UART bridge/control test failed: {serial}")
    if "shouldnotrunecho cleared" in serialtext:
        raise SystemExit("UART Ctrl-C did not cancel the unfinished command")
    print(f"PASS: {script.taps} taps, drag cancellation, editing, Ctrl-C, "
          f"shell restart, scrolling and UART forwarding; {out / 'screen.png'}")


if __name__ == "__main__":
    main()
