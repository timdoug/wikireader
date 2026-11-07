#!/usr/bin/env python3
"""GCC's testsuite for c33-linux-musl, its programs run in Linux in wremu.

    run.py [runtest selectors]	(default: the execute torture)

Repeats a DejaGnu pass in the VM (run-gcc.sh), which compiles the tests and
queues each program to execute that has no recorded result, and a batch in
wremu, booted through Grifo, which runs the queue in Linux and records how
each program ended and what it printed, until a pass queues nothing.  That
pass's gcc.sum and gcc.log, in artifacts/gcc-test/results, are the result.
"""
import fcntl
import importlib.util
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
# C33L_OUT gives a run its own queue, results and batch card, so runs of
# different suites can go side by side.
OUT = Path(os.environ.get("C33L_OUT", ROOT / "linux/artifacts/gcc-test")).resolve()
QUEUE = OUT / "queue"
EXECUTED = OUT / "executed"
# Seconds a test program may run, guest time: about the bare-metal board's
# 3.2 billion instructions.  pr97459's software 64-bit divisions take most
# of a minute.
TIMEOUT = int(os.environ.get("C33L_TIMEOUT", 240))
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
        "linux.img": OUT / "linux.img",
        "linux.ico": "linux/artifacts/linux.ico",
        "runner.bin": OUT / "runner",
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
        name, status, length, *count = header.decode().split()
        length = int(length)
        if len(rest) < length + 1:
            break
        output = rest[:length]
        rest = rest[length + 1:]
        # Coverage data the program wrote, kept beside its result at its
        # build path, for the board to put back.
        files = []
        for _ in range(int(count[0]) if count else 0):
            line, _, rest = rest.partition(b"\n")
            path, size = line.decode().rsplit(" ", 1)
            files.append((path, rest[:int(size)]))
            rest = rest[int(size) + 1:]
        for path, data in files:
            target = EXECUTED / (name + ".files") / path.lstrip("/")
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        (EXECUTED / name).write_bytes(status.encode() + b"\n" + output)
        (QUEUE / name).unlink()
        results = rest
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
    # One run at a time: they share the queue, the batch card and results.
    OUT.mkdir(parents=True, exist_ok=True)
    lock = (OUT / ".lock").open("w")
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        raise SystemExit(f"another run.py is using {OUT}")
    spec = importlib.util.spec_from_file_location("wr_app", ROOT / "linux/app-test.py")
    app = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(app)
    fat = app.load_fat_helper(ROOT)
    EXECUTED.mkdir(parents=True, exist_ok=True)
    vm("sh", "-c", "~/wr-linux/toolchain/install/bin/c33-linux-musl-gcc -O2 -Wall "
       f"-Werror linux/gcc-test/runner.c -o {OUT}/runner")
    # The programs run against the runtime libraries they were linked with,
    # the toolchain's, written over the image's in a copy of linux.img.
    vm("sh", "-c", f"""set -e
img={OUT}/linux.img rt=~/wr-linux/toolchain/install/c33-linux-musl/lib
cp linux/artifacts/linux.img "$img"
for f in libgcc_s.so.1 libatomic.so.1.2.0 $(cd "$rt" && ls libstdc++.so.6.0.*[0-9]); do
	~/wr-linux/toolchain/install/bin/c33-linux-musl-strip --strip-unneeded -o "$img.lib" "$rt/$f"
	chmod 755 "$img.lib"
	debugfs -w -R "rm /lib/$f" "$img" >/dev/null 2>&1
	debugfs -w -R "write $img.lib /lib/$f" "$img" >/dev/null 2>&1
	debugfs -w -R "sif /lib/$f uid 0" "$img" >/dev/null 2>&1
	debugfs -w -R "sif /lib/$f gid 0" "$img" >/dev/null 2>&1
	debugfs -R "dump /lib/$f /dev/stdout" "$img" 2>/dev/null | cmp -s - "$img.lib"
done
rm "$img.lib"
""")
    # C33L_PASSES stops after that many DejaGnu passes, for a summary
    # from results already recorded.
    for attempt in range(int(os.environ.get("C33L_PASSES", 8))):
        tool = next((a.split("=", 1)[1] for a in sys.argv[1:] if a.startswith("--tool=")), "gcc")
        # The runtime libraries' suites run from their build trees.
        script = "run-lib.sh" if tool in ("libstdc++", "libatomic") else "run-gcc.sh"
        print(vm("env", f"C33L_OUT={OUT}", f"linux/gcc-test/{script}", *sys.argv[1:]),
              end="", flush=True)
        names = sorted(p.name for p in QUEUE.iterdir())
        if not names:
            print(f"Results: {OUT / 'results' / (tool + '.sum')}")
            return
        done = 0
        while done < len(names):
            ran = run_batch(fat, names[done:done + BATCH])
            if ran == 0:
                raise SystemExit(f"the batch ran nothing; see {OUT / 'batch/boot.log'}")
            done += ran
        print(f"wremu ran {len(names)} queued programs", flush=True)
    raise SystemExit(f"still queuing after {attempt + 1} passes")


if __name__ == "__main__":
    main()
