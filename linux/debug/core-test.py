#!/usr/bin/env python3
"""Crash through Grifo, read SD artifacts, and symbolize on the host."""
import argparse
import importlib.util
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=root / "linux/artifacts/crashes")
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location("wr_app_test", root / "linux/app-test.py")
    app = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(app)
    fat = app.load_fat_helper(root)
    artifacts = root / "linux/artifacts"
    files = {name: app.require(path).read_bytes() for name, path in {
        "kernel.elf": root / "samo-lib/grifo/grifo.elf",
        "init.app": root / "samo-lib/grifo/applications/init/init.app",
        "linux.app": artifacts / "linux.app",
        "linux.img": artifacts / "linux.img",
        "linux.ico": artifacts / "linux.ico",
        "crashtst.bin": artifacts / "crashtst.bin",
        "crashlib.so": artifacts / "crashlib.so",
    }.items()}
    files["init.ini"] = (b"linux.ico : linux.app loglevel=7 earlycon=s1c33,mmio,0x300b00\n" * 2)
    args.output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="wr-linux-core-") as temporary:
        out = Path(temporary)
        card, flash, log, uart = (out / name for name in
                                 ("card.img", "flash.rom", "boot.log", "uart.in"))
        fat.make_image(card, files, 64)
        subprocess.run([sys.executable, str(root / "samo-lib/mbr/make-flash.py"),
                        str(flash)], check=True, stdout=subprocess.DEVNULL)
        # Return from each crashing child only after the kernel's collector
        # has saved its files. Test executable and DSO PCs, a zero limit and
        # a truncated capture without losing the shell or launcher.
        uart.write_text(
            "export LD_LIBRARY_PATH=/mnt/sd; "
            "/mnt/sd/crashtst.bin; "
            "cp /mnt/sd/crash.elf /mnt/sd/ecore.elf; "
            "cp /mnt/sd/crash.map /mnt/sd/ecore.map; "
            "cp /mnt/sd/crash.txt /mnt/sd/ecore.txt; "
            "/mnt/sd/crashtst.bin library; "
            "cp /mnt/sd/crash.elf /mnt/sd/lcore.elf; "
            "cp /mnt/sd/crash.map /mnt/sd/lcore.map; "
            "cp /mnt/sd/crash.txt /mnt/sd/lcore.txt; "
            "ulimit -S -c 0; /mnt/sd/crashtst.bin; "
            "cp /mnt/sd/crash.txt /mnt/sd/zcore.txt; "
            "ulimit -S -c 64; /mnt/sd/crashtst.bin; "
            "echo CORE CAPTURE PASS; reboot -f\n")
        command = [str(app.require(root / "emulator/wremu")), "-n", "3000000000",
                   "-T", "40,36,100000000", "--uart-input", str(uart),
                   "--uart-start", "850000000", "--uart-gap", "200000",
                   "-c", str(card), "-e", str(flash)]
        with log.open("w") as output:
            subprocess.run(command, cwd=out, stdout=output, stderr=subprocess.STDOUT,
                           check=True, timeout=300,
                           env={**os.environ, "WREMU_HOLD_MS": "33"})
        text = log.read_text(errors="replace")
        (args.output / "boot.log").write_text(text)
        if "CORE CAPTURE PASS" not in text or "watchdog reset" not in text or \
                "Kernel panic" in text or "elf_core_dump:" in text:
            print(text, file=sys.stderr)
            raise SystemExit("Core capture did not survive through reboot")
        for stem in ("ecore", "lcore", "crash"):
            for suffix in ("elf", "map", "txt"):
                name = f"{stem}.{suffix}"
                data = fat.read_file(card, name)
                if not data:
                    print(text, file=sys.stderr)
                    raise SystemExit(f"Core capture missing {name}")
                (args.output / name).write_bytes(data)
        if fat.read_file(card, "zcore.txt") != (args.output / "lcore.txt").read_bytes():
            raise SystemExit("RLIMIT_CORE=0 overwrote the last crash")
    for stem, expected in (("ecore", "crash_in_executable"), ("lcore", "crash_in_library")):
        result = subprocess.run([sys.executable, str(root / "linux/debug/core.py"),
                                 str(args.output / f"{stem}.elf")],
                                check=True, capture_output=True, text=True)
        (args.output / f"{stem}.symbols.txt").write_text(result.stdout)
        print(result.stdout, end="")
        source = "crash-test.c" if stem == "ecore" else "crash-library.c"
        if not any("PC:" in line and expected in line and re.search(re.escape(source) + r":\d+", line)
                   for line in result.stdout.splitlines()):
            raise SystemExit(f"Fault PC did not resolve to {expected} and its source line")
        if "SIGBUS" not in result.stdout:
            raise SystemExit("Linux signal number was not decoded as SIGBUS")
        if "status=complete" not in (args.output / f"{stem}.txt").read_text():
            raise SystemExit(f"Unexpected incomplete core: {stem}")
    # BusyBox's core ulimit unit is 512 bytes, so 64 blocks is 32 KiB.
    if "status=truncated" not in (args.output / "crash.txt").read_text() or \
            (args.output / "crash.elf").stat().st_size != 64 * 512:
        raise SystemExit("Small RLIMIT_CORE was not enforced")
    subprocess.run([sys.executable, str(root / "linux/debug/core.py"),
                    str(args.output / "crash.elf")], check=True,
                   stdout=subprocess.DEVNULL)
    with tempfile.TemporaryDirectory(prefix="wr-wrong-symbols-") as temporary:
        wrong = Path(temporary)
        (wrong / "build-id").write_text("wrong-build\n")
        result = subprocess.run([sys.executable, str(root / "linux/debug/core.py"),
                                 str(args.output / "ecore.elf"), "--symbols", str(wrong)],
                                capture_output=True, text=True)
        if result.returncode == 0 or "build IDs differ" not in result.stderr:
            raise SystemExit("Mismatched rootfs symbols were not rejected")
    print("Core passed: executable and shared-library SIGBUS PCs -> function/source line; "
          "zero and small core limits; shell survives; reboot through Grifo")
    print(f"Saved captures: {args.output}")


if __name__ == "__main__":
    main()
