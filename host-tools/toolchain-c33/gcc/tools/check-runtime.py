#!/usr/bin/env python3
"""Reject private unwinders in the installed dynamic C33 runtime."""
from pathlib import Path
import subprocess
import sys

prefix = Path(sys.argv[1])
target = "c33-linux-uclibc"
def run(tool, *args):
    return subprocess.check_output([str(prefix / "bin" / (target + "-" + tool)),
                                    *map(str, args)], text=True)

version = run("gcc", "-dumpfullversion").strip()
archive = prefix / "lib/gcc" / target / version / "libgcc.a"
members = run("ar", "t", archive).splitlines()
if any(member.startswith("unwind-") for member in members):
    raise SystemExit("libgcc.a contains EH objects: restore the shared runtime split")
if not (archive.parent / "libgcc_eh.a").is_file():
    raise SystemExit("Static libgcc_eh.a missing")
if len(sys.argv) > 2:
    elf = Path(sys.argv[2])
    symbols = run("nm", "--defined-only", elf)
    if any(line.split()[-1].split("@")[0].startswith(("_Unwind_", "__register_frame_info"))
           for line in symbols.splitlines() if line.split()):
        raise SystemExit(f"{elf}: private unwinder definitions")
    if "[libgcc_s.so.1]" not in run("readelf", "-d", elf):
        raise SystemExit(f"{elf}: C++ unwinder dependency missing")
print("C33 runtime guard passed")
