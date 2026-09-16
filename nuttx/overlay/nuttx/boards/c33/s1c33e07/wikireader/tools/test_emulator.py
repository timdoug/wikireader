#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run WikiReader NSH or ostest through the emulated console UART."""

import argparse
import hashlib
from pathlib import Path
import re
import subprocess

import wr_boot


def main():
    root = Path(__file__).resolve().parents[5]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wikireader", type=Path,
                        default=Path.home() / "wikireader")
    parser.add_argument("--image", type=Path, default=root / "nuttx")
    parser.add_argument("--out", type=Path)
    parser.add_argument("--ostest", action="store_true")
    args = parser.parse_args()

    emulator = args.wikireader.resolve() / "emulator/wremu"
    mode = "ostest" if args.ostest else "nsh"
    out = (args.out or root / "build/wikireader" / mode).resolve()
    out.mkdir(parents=True, exist_ok=True)

    # Keep the exact image with its transcript, even after another build.
    payload = args.image.read_bytes()
    if payload[:5] != b"\x7fELF\x01":
        raise SystemExit("Expected a 32-bit ELF image")
    image = out / "kernel.elf"
    image.write_bytes(payload)
    (out / "kernel.sha256").write_text(
        hashlib.sha256(payload).hexdigest() + "  kernel.elf\n")

    commands = ["echo C33_NSH_OK", "uname -a", "ps", "free",
                "sleep 1 &", "ps", "sleep 2", "echo C33_TIMER_OK"]
    if args.ostest:
        commands += ["ostest", "echo C33_OSTEST_RETURNED"]
    commands += ["echo C33_TEST_DONE"]
    source = out / "commands.txt"
    source.write_text("\n".join(commands) + "\n")

    transcript = out / "emulator.log"
    limit = str((20_000_000_000 if args.ostest else 500_000_000) +
                wr_boot.BOOT_CYCLES)
    card = out / "card.img"
    wr_boot.make_card(card, image, args.wikireader)
    flash = wr_boot.make_flash(out, args.wikireader)
    command = [str(emulator), "-n", limit, "--uart-input", str(source),
               "--uart-start", wr_boot.UART_START, "--uart-gap", "50000",
               *wr_boot.boot_args(card, flash)]
    with transcript.open("w") as log:
        result = subprocess.run(command, cwd=out, stdout=log,
                                stderr=subprocess.STDOUT, timeout=600)

    text = transcript.read_text(errors="replace").replace("\r", "")
    text = re.sub(r"\x1b\[[0-?]*[ -/]*[@-~]", "", text)
    (out / "console.txt").write_text(text)
    failures = []
    if result.returncode:
        failures.append(f"emulator exited with {result.returncode}")
    for marker in ("C33_NSH_OK", "C33_TIMER_OK", "C33_TEST_DONE"):
        if not re.search(r"^" + marker + r"$", text, re.M):
            failures.append(f"missing output: {marker}")
    # The shell task is "nsh_main" only when NuttX's own init entry point is
    # what started, which is what a direct ELF boot did.  Booted the way the
    # device boots, grifo chains to the board application, and that is the
    # task ps names as the shell's parent.
    for marker in ("NuttShell (NSH)", "Idle_Task", "wikireader_main", "Umem"):
        if marker not in text:
            failures.append(f"missing output: {marker}")
    if args.ostest:
        if "ostest_main: Exiting with status 0" not in text:
            failures.append("ostest did not report success")
        if not re.search(r"^C33_OSTEST_RETURNED$", text, re.M):
            failures.append("ostest did not return to the shell")
    if re.search(r"ERROR|PANIC|Assertion|stop reason:|command not found|"
                 r"nsh: .*failed", text):
        failures.append("guest or emulator reported an error")
    if failures:
        raise SystemExit("FAIL: " + "; ".join(failures) + f"\nSee {transcript}")
    print(f"PASS: WikiReader {mode}; transcript: {out / 'console.txt'}")


if __name__ == "__main__":
    main()
