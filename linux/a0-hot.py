#!/usr/bin/env python3
"""Choose the kernel's code and data for A0 RAM from wremu profiles.

A0 RAM (arch/c33/include/asm/iram.h) is the on-chip RAM the CPU reaches
without waiting: a function there fetches its instructions for nothing, and
an object there is read and written for nothing, where SDRAM costs about 1.7
cycles an instruction and 8 or 9 a data access.  This picks the input
sections that would save the most per byte, from wremu's PC profile (the
fetch wait column) and data profile (WREMU_DATAPROF), and writes the lists
the kernel's linker script includes:

    arch/c33/kernel/a0-text.lds   functions, for IVRAM in area 0 (12 KB)
    arch/c33/kernel/a0-data.lds   objects, for A0 RAM's own free part

Profile a kernel built with both lists empty (just their header comments),
so that every candidate is in SDRAM and shows what it costs there, with the
link map that build wrote (CONFIG_VMLINUX_MAP, linux/artifacts/vmlinux.map),
through Grifo, one window per workload, as the 2026-11-03 lists were:

    boot   start_kernel to devkmsg_write (wr-console's "prompt up")
    shell  a shell session from sys_sysinfo (its first command, free) to
           kernel_power_off
    X      linux/x11/run.py with WREMU_ARGS="-y 10000,999999 -F X.pc"

each with WREMU_KTEXT=<_etext> WREMU_DATAPROF=NAME.data and -F NAME.pc,
then run this with each workload as NAME:PCFILE:DATAFILE.  Each workload
counts as its share of its own window, so a byte that saves 1% of the boot
weighs what a byte that saves 1% of an X session does.  The linker script
asserts that what it is given fits.

Left out: code that runs when IVRAM is Grifo's again (process.o, the exit
to Grifo), idle and busy-wait loops, which only wait faster, and data in
drivers/, which may be a DMA buffer, which A0 RAM cannot be.  Sections
whose names start with two dots are the kernel's own special data.
"""
import argparse
import bisect
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent
KERNEL = ROOT / "overlay/arch/c33/kernel"
CODE_BYTES = 0x3000            # IVRAM in area 0: C33_A0_CODE_START..END
DATA_BYTES = 0x1fbc - 0xc00    # A0 RAM's own: C33_A0_DATA_START..END
CRC_TABLES = 4 * 512           # the SD driver's, from c33_iram_alloc()
SLACK = 64                     # alignment between input sections

CODE_SKIP_OBJECTS = ("arch/c33/kernel/process.o",)
CODE_SKIP_NAMES = re.compile(r"idle|putchar|delay")
DATA_OBJECTS = re.compile(r"^(kernel|mm|fs|lib|block|ipc|arch/c33)/")


def read_map(path):
    """Input sections as (start, size, section, object), from a GNU ld map."""
    out, pending = [], None
    text = Path(path).read_text().split("Linker script and memory map", 1)[1]
    for line in text.splitlines():
        m = re.match(r"^ (\.\S+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+\.o)$", line)
        if m:
            pending = None
        elif pending and (m2 := re.match(r"^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S+\.o)$", line)):
            m = (None, pending, *m2.groups())
            pending = None
        else:
            n = re.match(r"^ (\.\S+)$", line)
            pending = n.group(1) if n else None
            continue
        section, start, size, obj = m[1], int(m[2], 16), int(m[3], 16), m[4]
        if size and start:
            out.append((start, size, section, obj))
    return sorted(out)


def is_code(section, obj):
    if obj in CODE_SKIP_OBJECTS or CODE_SKIP_NAMES.search(section):
        return False
    return (section.startswith(".text.") and not section.startswith(".text.unlikely")
            or section in (".text", ".sched.text", ".spinlock.text"))


def is_data(section, obj):
    if not DATA_OBJECTS.match(obj):
        return False
    name = re.sub(r"^\.(data|bss|rodata)", "", section)
    return (section.split(".")[1] in ("data", "bss", "rodata")
            and not name.startswith("..") and name != section)


def charge(sections, starts, path, column, values, total_column=None):
    """Add each profile bucket's cost to the sections it overlaps."""
    total = 0
    width = 2 if column == 3 else 4
    for line in open(path):
        f = line.split()
        at, cost = int(f[0], 16), int(f[column])
        if total_column is not None:
            total += int(f[total_column])
        i = bisect.bisect_right(starts, at + width - 1) - 1
        while i >= 0 and sections[i][0] + sections[i][1] > at:
            start, size = sections[i][:2]
            overlap = min(at + width, start + size) - max(at, start)
            if overlap > 0:
                values[i] = values.get(i, 0) + cost * overlap / width
            if start <= at:
                break
            i -= 1
    return total


def choose(sections, scores, budget, wanted):
    """The sections that save the most within the budget: an exact 0/1
    knapsack in words, since saving per byte alone fills the budget with
    many small objects and leaves out the one large object worth more."""
    size = lambda i: sections[i][1]
    items = [i for i in scores if scores[i] > 0 and wanted(*sections[i][2:])
             and size(i) <= budget]
    words = budget // 4
    best = [0.0] * (words + 1)
    took = []
    for i in items:
        w = (size(i) + 3) // 4
        v = scores[i]
        row = bytearray(words + 1)
        for c in range(words, w - 1, -1):
            if best[c - w] + v > best[c]:
                best[c] = best[c - w] + v
                row[c] = 1
        took.append(row)
    picked, c = [], words
    for i, row in zip(reversed(items), reversed(took)):
        if row[c]:
            picked.append(i)
            c -= (size(i) + 3) // 4
    return picked, sum((size(i) + 3) & ~3 for i in picked)


def write(path, what, sections, picked, used, saved, names):
    lines = [f"/*\n * The kernel's hottest {what}, in A0 RAM (asm/iram.h): written by",
             " * linux/a0-hot.py from wremu profiles, which says how.",
             f" * {used} bytes, estimated to save " +
             ", ".join(f"{100 * s:.1f}% of {n}" for n, s in zip(names, saved)) + ".",
             " */"]
    order = (lambda i: sections[i][0]) if what == "functions" else (lambda i: -sections[i][1])
    for i in sorted(picked, key=order):
        start, size, section, obj = sections[i]
        # A file name without a wildcard would be linked a second time.
        lines.append(f"*{obj}({section})")
    Path(path).write_text("\n".join(lines) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--map", type=Path, default=ROOT / "artifacts/vmlinux.map",
                        help="the map of the build the profiles ran on")
    parser.add_argument("workload", nargs="+", help="NAME:PCFILE:DATAFILE")
    args = parser.parse_args()
    sections = read_map(args.map)
    starts = [s[0] for s in sections]
    names, code, data = [], [], []
    for w in args.workload:
        name, pc, dp = w.split(":")
        names.append(name)
        fetch, waits = {}, {}
        total = charge(sections, starts, pc, 3, fetch, total_column=2)
        charge(sections, starts, dp, 2, waits)
        code.append({i: v / total for i, v in fetch.items()})
        data.append({i: v / total for i, v in waits.items()})
    mean = lambda shares: {i: sum(s.get(i, 0) for s in shares) / len(shares)
                           for i in set().union(*shares)}
    iram = sum(s[1] for s in sections if s[2] == ".iram.text")
    for what, shares, budget, wanted, out in (
            ("functions", code, CODE_BYTES - iram - SLACK, is_code, KERNEL / "a0-text.lds"),
            ("data", data, DATA_BYTES - CRC_TABLES - 4 - SLACK, is_data, KERNEL / "a0-data.lds")):
        picked, used = choose(sections, mean(shares), budget, wanted)
        saved = [sum(s.get(i, 0) for i in picked) for s in shares]
        write(out, what, sections, picked, used, saved, names)
        print(f"{what}: {len(picked)} sections, {used} of {budget} bytes, saving " +
              ", ".join(f"{100 * s:.1f}% of {n}" for n, s in zip(names, saved)))


if __name__ == "__main__":
    main()
