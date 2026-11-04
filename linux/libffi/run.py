#!/usr/bin/env python3
"""Run libffi's own call tests on the C33 port through Grifo.

linux/libffi/build.sh builds them, and the libffi they link against, which
rides in the same archive because the system image has none.  A test passes when it exits 0 (libffi's
CHECK aborts on a wrong value).  call-overread needs mprotect guard pages,
which a machine without an MMU cannot give, and is expected to fail.
"""
import argparse
import importlib.util
import io
import os
from pathlib import Path
import subprocess
import sys
import tarfile

ROOT = Path(__file__).resolve().parents[2]
EXPECTED_FAILURES = {"call-overread"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "linux/artifacts/libffi-run")
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    spec = importlib.util.spec_from_file_location("wr_app", ROOT / "linux/app-test.py")
    app = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(app)
    fat = app.load_fat_helper(ROOT)
    files = {name: app.require(path).read_bytes() for name, path in {
        "kernel.elf": ROOT / "samo-lib/grifo/grifo.elf",
        "init.app": ROOT / "samo-lib/grifo/applications/init/init.app",
        "linux.app": ROOT / "linux/artifacts/linux.app",
        "linux.img": ROOT / "linux/artifacts/linux.img",
        "linux.ico": ROOT / "linux/artifacts/linux.ico",
        "ffitests.tar": ROOT / "linux/artifacts/libffi-tests/ffitests.tar",
    }.items()}
    files["init.ini"] = b"linux.ico : linux.app\n"
    files["ffirun.sh"] = b"""#!/bin/sh
mkdir -p /tmp/ffi
tar -xf /mnt/sd/ffitests.tar -C /tmp/ffi
cd /tmp/ffi/bin
export LD_LIBRARY_PATH=/tmp/ffi/lib
: > /mnt/sd/ffires.txt
for t in *; do
    ./$t > /dev/null 2>&1
    echo "$t $?" >> /mnt/sd/ffires.txt
done
sync
echo FFI DONE
poweroff
"""
    fat.make_image(out / "card.img", files, 64)
    (out / "flash.rom").unlink(missing_ok=True)
    subprocess.run([sys.executable, str(ROOT / "samo-lib/mbr/make-flash.py"),
                    str(out / "flash.rom")], check=True, stdout=subprocess.DEVNULL)
    (out / "uart.in").write_text("sh /mnt/sd/ffirun.sh\n")
    with (out / "boot.log").open("w") as log:
        subprocess.run([str(ROOT / "emulator/wremu"), "-n", "60000000000",
                        "-e", str(out / "flash.rom"), "-c", str(out / "card.img"),
                        "--uart-input", str(out / "uart.in"), "--uart-start", "600000000"],
                       cwd=out, stdout=log, stderr=subprocess.STDOUT, check=True,
                       timeout=5400, env=os.environ)
    results = fat.read_file(out / "card.img", "ffires.txt")
    if not results:
        raise SystemExit(f"No results; see {out / 'boot.log'}")
    (out / "ffires.txt").write_bytes(results)
    status = dict(line.rsplit(" ", 1) for line in results.decode().splitlines())
    failed = {name for name, rc in status.items() if rc != "0"}
    with tarfile.open(ROOT / "linux/artifacts/libffi-tests/ffitests.tar") as tar:
        built = {Path(m.name).name for m in tar.getmembers()
                 if m.isfile() and Path(m.name).parent.name == "bin"}
    if built - status.keys() or failed != EXPECTED_FAILURES & built:
        raise SystemExit(f"libffi: failed {sorted(failed)}, not run {sorted(built - status.keys())}")
    print(f"libffi passed: {len(status) - len(failed)} of {len(status)} tests,"
          f" {', '.join(sorted(failed))} failing as expected without an MMU")


if __name__ == "__main__":
    main()
