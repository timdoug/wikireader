#!/usr/bin/env python3
"""Run standalone LTP POSIX tests through FLASH -> Grifo -> launcher -> Linux."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[2]
STATUS = {0: "PASS", 1: "FAIL", 2: "UNRESOLVED", 4: "UNSUPPORTED", 5: "UNTESTED", 124: "TIMEOUT"}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binaries", type=Path, default=ROOT / "linux/artifacts/ltp")
    parser.add_argument("--output", type=Path, default=ROOT / "linux/artifacts/ltp-run")
    parser.add_argument("--start", type=int, default=0)
    parser.add_argument("--count", type=int)
    parser.add_argument("--case", action="append", help="Run this named case (repeatable)")
    parser.add_argument("--timeout", type=int, default=30, help="Guest seconds per case")
    parser.add_argument("--timeouts", type=Path, help="JSON object of per-case guest deadlines")
    parser.add_argument("--wall-timeout", type=int, default=600)
    parser.add_argument("--rootfs", type=Path, default=ROOT / "linux/artifacts/linux.img")
    parser.add_argument("--kernel", type=Path, default=ROOT / "linux/artifacts/linux.app")
    parser.add_argument("--emulator", type=Path, default=ROOT / "emulator/wremu")
    parser.add_argument("--trace-sd", action="store_true", help="Keep SD command diagnostics in the boot log")
    args = parser.parse_args()
    deadlines = json.loads(args.timeouts.read_text()) if args.timeouts else {}
    if args.timeout <= 0 or any(type(n) is not int or n <= 0 for n in deadlines.values()):
        raise SystemExit("Guest deadlines must be positive integer seconds")
    args.binaries = args.binaries.resolve()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    spec = importlib.util.spec_from_file_location("wr_app_test", ROOT / "linux/app-test.py")
    app = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(app)
    fat = app.load_fat_helper(ROOT)
    manifest = json.loads((args.binaries / "manifest.json").read_text())
    cases = manifest["cases"]
    if args.case:
        selected = set(args.case)
        cases = [case for case in cases if case["name"] in selected]
        if len(cases) != len(selected):
            raise SystemExit("A selected case is absent from the build manifest")
    else:
        cases = cases[args.start:args.start + args.count if args.count is not None else None]
    if not cases:
        raise SystemExit("No cases selected")
    manifest = {**manifest, "cases": cases}
    files = {name: app.require(ROOT / path).read_bytes() for name, path in {
        "kernel.elf": "samo-lib/grifo/grifo.elf", "init.app": "samo-lib/grifo/applications/init/init.app",
        "linux.ico": "linux/artifacts/linux.ico",
    }.items()}
    files["linux.app"] = app.require(args.kernel).read_bytes()
    files["linux.img"] = app.require(args.rootfs).read_bytes()
    for fixture in manifest.get("fixtures", []):
        data = (args.binaries / fixture["binary"]).read_bytes()
        if digest(data) != fixture["binary_sha256"]:
            raise SystemExit("Fixture hash mismatch")
        files[fixture["binary"]] = data
    supervisor = manifest["supervisor"]
    files[supervisor["binary"]] = (args.binaries / supervisor["binary"]).read_bytes()
    if digest(files[supervisor["binary"]]) != supervisor["binary_sha256"]:
        raise SystemExit("Supervisor hash mismatch")
    commands = ["#!/bin/sh", "mkdir -p /tmp/ltp; cd /tmp/ltp"]
    for index, case in enumerate(cases):
        data = (args.binaries / case["binary"]).read_bytes()
        if digest(data) != case["binary_sha256"]:
            raise SystemExit(f"Binary hash mismatch: {case['name']}")
        files[case["binary"]] = data
        # Status comes from the standalone program, not its printed messages.
        commands += [f"mkdir c{index}; cd c{index}", f"echo LTP-BEGIN {case['name']}",
                     f"/mnt/sd/{supervisor['binary']} {deadlines.get(case['name'], args.timeout)} /mnt/sd/{case['binary']}",
                     "status=$?", f"echo LTP-RESULT {case['name']} $status", "cd ..",
                     f"rm -rf c{index}"]
    commands += ["echo LTP-DONE", "reboot -f"]
    files["ltp.sh"] = ("\n".join(commands) + "\n").encode()
    files["init.ini"] = b"linux.ico : linux.app loglevel=7 earlycon=s1c33,mmio,0x300b00\n" * 2
    card, flash, uart = (out / name for name in ("card.img", "flash.rom", "uart.in"))
    size_mb = 64
    while sum(map(len, files.values())) > size_mb * 1024 * 1024 * 0.8:
        size_mb *= 2
    fat.make_image(card, files, size_mb)
    # make-flash refuses existing outputs; this is our generated fixture.
    flash.unlink(missing_ok=True)
    subprocess.run([sys.executable, str(ROOT / "samo-lib/mbr/make-flash.py"), str(flash)],
                   check=True, stdout=subprocess.DEVNULL)
    uart.write_text("sh /mnt/sd/ltp.sh\n")
    # Leave enough instruction budget for the guest deadlines, including
    # idle advances; the independent host deadline still bounds the run.
    instruction_budget = max(100000000000, 120000000 *
                             sum(deadlines.get(c["name"], args.timeout) for c in cases))
    emulator = app.require(args.emulator.resolve())
    emulator_sha256 = digest(emulator.read_bytes())
    command = [str(emulator), "-n", str(instruction_budget), "-T", "40,36,100000000",
               "--uart-input", str(uart), "--uart-start", "850000000", "--uart-gap", "200000",
               "-c", str(card), "-e", str(flash)]
    if args.trace_sd:
        command.append("--trace-sd")
    host_error = None
    environment = {**os.environ, "WREMU_HOLD_MS": "33"}
    with (out / "boot.log").open("w") as log:
        process = subprocess.Popen(command, cwd=out, stdout=log, stderr=subprocess.STDOUT,
                                   env=environment)
        deadline = time.monotonic() + args.wall_timeout
        try:
            while process.poll() is None:
                text = (out / "boot.log").read_text(errors="replace")
                reset = text.find("watchdog reset", text.find("LTP-DONE")) if "LTP-DONE" in text else -1
                if reset >= 0 and "init choosing" in text[reset:]:
                    process.terminate()
                    break
                if reset >= 0 and "load 'kernel.elf' error" in text[reset:]:
                    host_error = "Grifo reload failed after the trial"
                    process.terminate()
                    break
                if time.monotonic() >= deadline:
                    host_error = "Host deadline expired"
                    process.terminate()
                    break
                time.sleep(0.2)
            process.wait(timeout=5)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
        if process.returncode not in (0, -15):
            host_error = f"Emulator exit {process.returncode}"
    raw_text = (out / "boot.log").read_text(errors="replace").replace("\r", "")
    # Host emulator notices may interrupt the guest's serial output.
    text = re.sub(r" *\[[a-z][^\]\n]*\]\n?", "", raw_text)
    results = []
    for case in cases:
        matches = re.findall(r"^LTP-RESULT " + re.escape(case["name"]) + r" (\d+)\s*$", text, re.M)
        code = int(matches[0]) if len(matches) == 1 else None
        status = STATUS.get(code, "MISSING" if code is None else "ERROR")
        results.append({"name": case["name"], "status": status, "exit_status": code})
        print(f"{status}: {case['name']} (exit {code})")
    complete = (host_error is None and "LTP-DONE" in text and "watchdog reset" in raw_text
                and "C33 boot: Grifo application" in text and "init choosing" in text
                and "Kernel panic" not in text and all(r["exit_status"] is not None for r in results))
    report = {"build": manifest, "guest_timeout": args.timeout,
              "guest_timeouts": {c["name"]: deadlines.get(c["name"], args.timeout) for c in cases},
              "fixture_sha256": {name: digest(data) for name, data in files.items()},
              "emulator_sha256": emulator_sha256,
              "environment": {k: v for k, v in environment.items() if k.startswith("WREMU_")},
              "command": command, "complete": complete, "host_error": host_error, "results": results}
    (out / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    if not complete or any(r["status"] != "PASS" for r in results):
        raise SystemExit(f"LTP smoke trial needs attention; see {out / 'boot.log'}")


if __name__ == "__main__":
    main()
