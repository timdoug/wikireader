#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Read and write a card from NuttX's own SD driver.

The card slot is the one piece of the machine NuttX used to leave entirely to
Grifo: the kernel read the application off the card and NuttX never touched
it again.  It now drives the slot itself -- the chip's SPI master, the card's
power rail on port 3, the chip select on P50, and the two high-speed DMA
channels that carry a block -- so the card has to be exercised from the guest
rather than assumed.

The fixture is the same pure-Python FAT32 builder the launcher test uses: an
MBR with one FAT32 partition, which is how a WikiReader card is laid out.
What it checks:

  * the partition table is read and the first partition mounted;
  * a file written before the run reads back byte for byte, via md5sum in the
    guest against the same digest computed here;
  * a file the guest writes is on the image afterwards, read back by this
    script from the raw image rather than from anything the guest said.

The payload is large enough that most of it moves through the DMA path.  The
emulator counts the engines' transfers -- one 32-bit word each, so 128 to a
block -- which makes a run that quietly fell back to the byte-at-a-time path
visible rather than merely slower.

Every command here also works typed at the device's prompt, which is the
point: a run on hardware compares against the same transcript.
"""

import argparse
import hashlib
import os
from pathlib import Path
import re
import subprocess
import sys

import wr_boot

# Written by the guest, read back from the image by this script.
RESULT_NAME = "result.txt"
RESULT_TEXT = "nuttx wrote this"

PAYLOAD_NAME = "payload.bin"
PAYLOAD_SIZE = 64 * 1024


def payload():
    """Compressible-looking but non-repeating, so a shifted block shows up."""

    data = bytearray()
    seed = 0x12345678
    while len(data) < PAYLOAD_SIZE:
        seed = (seed * 1103515245 + 12345) & 0xffffffff
        data += seed.to_bytes(4, "little")
    return bytes(data[:PAYLOAD_SIZE])




def run(command, out, name):
    with (out / name).open("w") as log:
        subprocess.run(command, cwd=out, stdout=log, stderr=subprocess.STDOUT,
                       check=True, timeout=1800,
                       env={**os.environ, "WREMU_HOLD_MS": "33"})
    text = (out / name).read_bytes().decode(errors="replace").replace("\r", "")
    if re.search(r"Panic:|PANIC|Assertion|unmapped|misaligned", text):
        raise SystemExit(f"Guest failure: see {out / name}")
    return re.sub(r"\x1b\[[0-?]*[ -/]*[@-~]", "", text)


def main():
    root = Path(__file__).resolve().parents[5]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wikireader", type=Path,
                        default=Path.home() / "wikireader")
    parser.add_argument("--image", type=Path, default=root / "nuttx")
    parser.add_argument("--out", type=Path,
                        default=root / "build/wikireader/sdcard")
    parser.add_argument("--limit", type=int,
                        default=1_500_000_000 + wr_boot.BOOT_CYCLES)
    args = parser.parse_args()

    wikireader = args.wikireader.resolve()
    emulator = wikireader / "emulator/wremu"
    if not emulator.exists():
        print(f"SKIP: {emulator} is missing; build the emulator first")
        return 0

    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    card = out / "card.img"
    content = payload()
    digest = hashlib.md5(content).hexdigest()
    fat = wr_boot.make_card(card, args.image.resolve(), wikireader,
                            {PAYLOAD_NAME: content})
    flash = wr_boot.make_flash(out, wikireader)
    if fat.read_file(card, PAYLOAD_NAME) != content:
        raise SystemExit("Card image did not read back its own payload")

    # The unmount is what forces the directory entry and the FAT out to the
    # card; without it the write would only ever have existed in the cache.
    commands = "".join(line + "\n" for line in [
        "mount",
        f"md5sum /sd/{PAYLOAD_NAME}",
        f"echo {RESULT_TEXT} > /sd/{RESULT_NAME}",
        f"cat /sd/{RESULT_NAME}",
        "umount /sd",
    ])
    source = out / "input.txt"
    source.write_text(commands)

    text = run([str(emulator), "-n", str(args.limit),
                *wr_boot.boot_args(card, flash),
                "--uart-input", str(source),
                "--uart-start", wr_boot.UART_START,
                "--uart-gap", "400000"],
               out, "sdcard.log")
    (out / "console.txt").write_text(text)

    problems = []
    if not re.search(r"^\s+/sd type vfat$", text, re.M):
        problems.append("the first partition did not mount as vfat")
    if digest not in text:
        problems.append(f"md5sum of {PAYLOAD_NAME} did not match {digest}")
    if RESULT_TEXT not in text.split("cat /sd/")[-1]:
        problems.append(f"the guest could not read back {RESULT_NAME}")

    written = fat.read_file(card, RESULT_NAME)
    if written is None:
        problems.append(f"{RESULT_NAME} is not on the card image")
    elif written.decode(errors="replace").strip() != RESULT_TEXT:
        problems.append(f"{RESULT_NAME} holds {written!r}")

    blocks = re.search(r"--- sd: \d+ commands, (\d+) blocks read, "
                       r"(\d+) written", text)
    receive = re.search(r"HSDMA2 TX (\d+), HSDMA3 RX (\d+)", text)
    if blocks and int(blocks.group(2)) == 0:
        problems.append("nothing was written to the card at all")
    if receive and int(receive.group(2)) == 0:
        problems.append("no block came in over DMA")
    if re.search(r"DMA stalled", text):
        problems.append("a DMA transfer stalled and fell back to the CPU")

    if problems:
        for problem in problems:
            print(f"   {problem}")
        raise SystemExit(f"see {out / 'console.txt'}")

    print(f"PASS: mounted, {blocks.group(1)} blocks read and "
          f"{blocks.group(2)} written, {receive.group(2)} words of "
          f"{int(blocks.group(1)) * 128} over DMA; {out / 'console.txt'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
