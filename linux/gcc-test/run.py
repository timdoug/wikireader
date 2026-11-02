#!/usr/bin/env python3
"""GCC's testsuite for c33-linux-uclibc, its programs run in Linux in wremu.

    run.py [runtest selectors]	(default: the execute torture)

Repeats a DejaGnu pass in the VM (run-gcc.sh), which compiles the tests and
queues each program to execute that has no recorded result, and a batch in
wremu, booted through Grifo, which runs the queue in Linux and records how
each program ended and what it printed, until a pass queues nothing.  That
pass's gcc.sum and gcc.log, in artifacts/gcc-test/results, are the result.
"""
import importlib.util
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "linux/artifacts/gcc-test"
QUEUE = OUT / "queue"
EXECUTED = OUT / "executed"
# Seconds a test program may run, guest time.
TIMEOUT = 60
# Programs a card holds and a boot runs.
BATCH = 3000
VM = ["limactl", "shell", "--workdir", str(ROOT), "wr-linux"]


def vm(*command):
    return subprocess.run([*VM, *command], check=True, text=True,
                          stdout=subprocess.PIPE).stdout


def run_batch(fat, names):
    """Run NAMES in one boot; return how many ran.  A program that takes
    the machine down is recorded as having crashed it, and counts."""
    batch = OUT / "batch"
    batch.mkdir(parents=True, exist_ok=True)
    archive = bytearray()
    for name in names:
        program = (QUEUE / name).read_bytes()
        archive += f"{name} {len(program)}\n".encode() + program
    files = {name: (ROOT / path).read_bytes() for name, path in {
        "kernel.elf": "samo-lib/grifo/grifo.elf",
        "init.app": "samo-lib/grifo/applications/init/init.app",
        "linux.app": "linux/artifacts/linux.app",
        "linux.img": "linux/artifacts/linux.img",
        "linux.ico": "linux/artifacts/linux.ico",
        "runner.bin": "linux/artifacts/gcc-test/runner",
    }.items()}
    files["init.ini"] = b"linux.ico : linux.app wr.blank=0\n"
    files["tests.bin"] = bytes(archive)
    files["gt.sh"] = (f"/mnt/sd/runner.bin /mnt/sd/tests.bin /mnt/sd/results.txt "
                      f"{TIMEOUT}\nsync\npoweroff\n").encode()
    card = batch / "card.img"
    fat.make_image(card, files, 16)
    (batch / "flash.rom").unlink(missing_ok=True)
    subprocess.run([sys.executable, str(ROOT / "samo-lib/mbr/make-flash.py"),
                    str(batch / "flash.rom")], check=True, stdout=subprocess.DEVNULL)
    (batch / "uart.in").write_text("sh /mnt/sd/gt.sh\n")
    # Each program takes well under a second of guest time; the budget only
    # bounds a guest that never finishes, and a halted one fast-forwards.
    budget = 60_000_000 * (60 + len(names) * (TIMEOUT + 5))
    with (batch / "boot.log").open("w") as log:
        exited = subprocess.run([str(ROOT / "emulator/wremu"), "-n", str(budget),
                                 "-e", str(batch / "flash.rom"), "-c", str(card),
                                 "--uart-input", str(batch / "uart.in"),
                                 "--uart-start", "600000000"],
                                cwd=batch, stdout=log, stderr=subprocess.STDOUT)
    console = (batch / "boot.log").read_bytes()
    if b"GCCTEST START" not in console:
        raise SystemExit(f"the runner never started; see {batch / 'boot.log'}")
    results = fat.read_file(card, "results.txt") or b""
    done = 0
    while results:
        header, _, rest = results.partition(b"\n")
        name, status, length = header.decode().split()
        length = int(length)
        if len(rest) < length + 1:
            break
        (EXECUTED / name).write_bytes(status.encode() + b"\n" + rest[:length])
        (QUEUE / name).unlink()
        results = rest[length + 1:]
        done += 1
    if done < len(names) and b"GCCTEST DONE" not in console:
        name = names[done]
        print(f"{name} crashed the machine (wremu exit {exited.returncode})",
              flush=True)
        (EXECUTED / name).write_bytes(b"crash\n")
        (QUEUE / name).unlink()
        done += 1
    return done


def main():
    spec = importlib.util.spec_from_file_location("wr_app", ROOT / "linux/app-test.py")
    app = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(app)
    fat = app.load_fat_helper(ROOT)
    EXECUTED.mkdir(parents=True, exist_ok=True)
    vm("sh", "-c", "~/wr-linux/toolchain/install/bin/c33-linux-uclibc-gcc -O2 -Wall "
       "-Werror linux/gcc-test/runner.c -o linux/artifacts/gcc-test/runner")
    for attempt in range(8):
        print(vm("linux/gcc-test/run-gcc.sh", *sys.argv[1:]), end="", flush=True)
        names = sorted(p.name for p in QUEUE.iterdir())
        if not names:
            print(f"Results: {OUT / 'results/gcc.sum'}")
            return
        done = 0
        while done < len(names):
            ran = run_batch(fat, names[done:done + BATCH])
            if ran == 0:
                raise SystemExit(f"the batch ran nothing; see {OUT / 'batch/boot.log'}")
            done += ran
        print(f"wremu ran {len(names)} queued programs", flush=True)
    raise SystemExit("still queuing after 8 passes")


if __name__ == "__main__":
    main()
