#!/usr/bin/env python3
"""Boot Linux from Grifo's tiled launcher and require a reboot back to it."""

import argparse
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


# wr.selftest runs the userspace checks rcS skips on a normal boot.  The
# console is quiet by default, and early parameters take effect in order, so
# loglevel=7 comes first or earlycon's own announcement is filtered out.
LAUNCHER_ARGS = (b"loglevel=7 earlycon=s1c33,mmio,0x300b00 wr.blank=5"
                 b" wr.selftest")


def suspend_run(root, emulator, files, make_flash, fat):
    """A single-entry card auto-boots Linux; leave it alone until it sleeps."""
    with tempfile.TemporaryDirectory(prefix="wr-linux-suspend-") as temporary:
        out = Path(temporary)
        card = out / "card.img"
        flash = out / "flash.rom"
        log = out / "boot.log"
        uart = out / "uart.in"
        uart.write_text("/mnt/sd/tlssusp.bin\n")
        card_files = dict(files)
        # wr.pmlog asks for the breadcrumbs this test reads back off the card.
        # loglevel=7 because the PM markers below are pr_info.
        card_files["init.ini"] = (
            b"linux.ico : linux.app wr.blank=2 wr.suspend=30 wr.pmlog"
            b" s1c33_wake=1 loglevel=7\n")
        fat.make_image(card, card_files, 64)
        subprocess.run([sys.executable, str(make_flash), str(flash)],
                       check=True, stdout=subprocess.DEVNULL)
        command = [
            str(emulator), "-n", "3000000000",
            # Nothing else runs, so the guest idles into suspend long before
            # this tap; wremu advances an idle machine to its next scripted
            # event, which is the touch that has to wake it.
            "-T", f"{ICON0[0]},{ICON0[1]},2500000000",
            "--uart-input", str(uart), "--uart-start", "850000000",
            "--uart-gap", "200000",
            "-c", str(card), "-e", str(flash),
        ]
        with log.open("w") as output:
            subprocess.run(command, cwd=out, stdout=output,
                           stderr=subprocess.STDOUT, check=True, timeout=600,
                           env={**os.environ, "WREMU_HOLD_MS": "33"})
        text = log.read_bytes().decode(errors="replace").replace("\r", "")
        # wremu writes its own scripted-event traces into the same stream,
        # which can land in the middle of a line the guest was printing.
        text = re.sub(r" *\[[a-z][^\]]*\]\n?", "", text)
        notes = fat.read_file(card, "linuxpm.txt") or b""

    # The interrupt that ends a suspend is taken as the wake event rather than
    # handled, so unless it is replayed afterwards its character is never read
    # and the port it arrived on never receives again: a machine that wakes
    # once and then ignores the panel.
    touches = [int(n) for n in
               re.findall(r"^\s*\d+:\s+(\d+).*s1c33-uart1-rx",
                          notes.decode(errors="replace"), re.M)]
    if len(touches) < 2 or touches[1] <= touches[0]:
        print(notes.decode(errors="replace"), file=sys.stderr)
        raise SystemExit("Suspend regression failed: the touch interrupt that "
                         f"woke the machine was never delivered: {touches}")

    expected = [
        "C33 power: suspending until touch",
        "PM: suspend entry (s2idle)",
        "PM: suspend exit",
        "C33 power: resumed",
        "C33 display: woken by touch",
        "TLS SUSPEND PASS",
    ]
    missing = [marker for marker in expected if marker not in text]
    ready = text.find("TLS SUSPEND READY")
    asleep = text.find("PM: suspend entry (s2idle)", ready)
    # Userspace thaws before the final "PM: suspend exit" printk.
    resumed = text.find("Restarting tasks: Starting", asleep)
    passed = text.find("TLS SUSPEND PASS", resumed)
    if not (0 <= ready < asleep < resumed < passed):
        missing.append("same-thread TLS check spanning suspend")
    # Channel 3 is the wake timer armed across suspend; it has to keep firing
    # while the tick is frozen, or nothing would re-check the wake sources.
    if not re.search(r"ch3 A \d+ B [1-9]", text):
        missing.append("suspend wake timer matches")
    if missing or re.search(r"Kernel panic|suspend REFUSED|TLS SUSPEND FAIL", text):
        print(text, file=sys.stderr)
        raise SystemExit("Linux suspend regression failed; missing: " +
                         ", ".join(missing or ["a clean suspend"]))
    print("Suspend passed: idle -> s2idle -> touch -> resume; live-thread TLS intact")


def main():
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", type=Path, default=root / "linux/artifacts/linux.app")
    parser.add_argument("--rootfs", type=Path, default=root / "linux/artifacts/linux.img")
    args = parser.parse_args()
    emulator = require(root / "emulator/wremu")
    grifo = require(root / "samo-lib/grifo/grifo.elf")
    launcher = require(root / "samo-lib/grifo/applications/init/init.app")
    app = require(args.kernel)
    system = require(args.rootfs)
    pthread_test = require(root / "linux/artifacts/pthread-test")
    pthread_static = require(root / "linux/artifacts/pthread-test-static")
    signal_test = require(root / "linux/artifacts/signal-test")
    relocation_test = require(root / "linux/artifacts/relocation-test")
    origin_rpath = require(root / "linux/artifacts/origin-rpath")
    origin_runpath = require(root / "linux/artifacts/origin-runpath")
    origin_library = require(root / "linux/artifacts/origin.so")
    exec_stack_test = require(root / "linux/artifacts/exec-stack-test")
    cxx_test = require(root / "linux/artifacts/cxx-test")
    atomic_test = require(root / "linux/artifacts/atomic-test")
    ipc_test = require(root / "linux/artifacts/ipc-test")
    native_tls = require(root / "linux/artifacts/nptl-test")
    tls_library = require(root / "linux/artifacts/tls-library.so")
    tls_late = require(root / "linux/artifacts/tls-late-library.so")
    tls_exec = require(root / "linux/artifacts/tls-exec-test")
    tls_suspend = require(root / "linux/artifacts/tls-suspend-test")
    unwind_library = require(root / "linux/artifacts/unwind-library.so")
    unwind_legacy = require(root / "linux/artifacts/unwind-legacy-library.so")
    runtime_bench = require(root / "linux/artifacts/runtime-bench")
    card_test = require(root / "linux/card/bin/t")
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
            # The system itself, which linux.app mounts from here.
            "linux.img": system.read_bytes(),
            "linux.ico": icon.read_bytes(),
            # The thread, atomic and C++ tests are not in the system image, so
            # they ride here.
            "pthtest.bin": pthread_test.read_bytes(),
            "pthstat.bin": pthread_static.read_bytes(),
            "sigtest.bin": signal_test.read_bytes(),
            "reloc.bin": relocation_test.read_bytes(),
            "origr.bin": origin_rpath.read_bytes(),
            "orign.bin": origin_runpath.read_bytes(),
            "origin.so": origin_library.read_bytes(),
            "execstk.bin": exec_stack_test.read_bytes(),
            "cxxtest.bin": cxx_test.read_bytes(),
            "atomtest.bin": atomic_test.read_bytes(),
            "ipctest.bin": ipc_test.read_bytes(),
            "nptl.bin": native_tls.read_bytes(),
            "tlslib.so": tls_library.read_bytes(),
            "tlslate.so": tls_late.read_bytes(),
            "tlsexec.bin": tls_exec.read_bytes(),
            "tlssusp.bin": tls_suspend.read_bytes(),
            "unwind.so": unwind_library.read_bytes(),
            "unwind0.so": unwind_legacy.read_bytes(),
            "bench.bin": runtime_bench.read_bytes(),
            "t.sh": card_test.read_bytes(),
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
        # One line: hush discards type-ahead each time it prompts.
        uart_input.write_text("mkdir -p /mnt/sd/bin && cp /mnt/sd/t.sh /mnt/sd/bin/t && "
                              "/mnt/sd/reloc.bin && /mnt/sd/origr.bin && /mnt/sd/orign.bin && "
                              "/mnt/sd/execstk.bin && "
                              "t && /mnt/sd/sigtest.bin && cat /mnt/sd/thread.txt && ipcs -a && "
                              "echo C33 LINUX APP PASS; reboot -f\n")

        command = [
            str(emulator), "-n", "2000000000",
            "-T", f"{ICON0[0]},{ICON0[1]},100000000",
            "--uart-input", str(uart_input),
            "--uart-start", "850000000", "--uart-gap", "200000",
            # Writable: the system's ext4 image is on this card.
            "-c", str(card), "-e", str(flash),
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
            "C33 BusyBox init: kernel self-test passed",
            "NPTL PASS",
            "RELOCATION PASS",
            "ORIGIN PASS",
            "EXEC STACK PASS",
            "SIGNAL PASS:",
            "PTHREAD PASS",
            "STATIC PTHREAD PASS",
            "ATOMIC PASS",
            "IPC PASS",
            "CXX PASS",
            "BENCH PASS",
            "PASS (thread.txt)",
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

    print("Userspace passed: NPTL/native ELF TLS, pthreads, C11/GCC atomics, "
          "cross-process IPC and C++")
    print("Launcher Linux passed: Grifo menu -> linux.app -> BusyBox -> "
          "reboot -> Grifo menu")
    suspend_run(root, emulator, files, make_flash, fat)
    return 0


if __name__ == "__main__":
    sys.exit(main())
