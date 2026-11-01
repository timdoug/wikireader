#!/usr/bin/env python3
"""Compare the two builds in a zimbench.txt written by the device benchmark.

Uses the most recent run of each label ("old" and "new"), and for every
article the fastest of its measured passes (pass 0 only warms the caches).
The spread column is the slowest measured pass over the fastest, for the new
build, to show how far one sample can be trusted.
"""
import re
import sys

FIELDS = ("retrieve_us", "screen_us", "image_us")


def runs(text):
    """{label: [record, ...]} for the last run of each label."""
    last = {}
    current = None
    for line in text.splitlines():
        if line.startswith("ZBRUN "):
            current = re.search(r"label=(\S+)", line).group(1)
            last[current] = []
        elif line.startswith("ZB ") and current and "missing=1" not in line:
            rec = dict(re.findall(r"(\w+)=(\S+)", line.split(" title=")[0]))
            rec["title"] = re.search(r" title=(.*?) retrieve_us=", line).group(1)
            rec["archive"] = rec["archive"].split("/")[-1]
            for f in FIELDS + ("pass", "images", "read_us"):
                rec[f] = int(re.search(r" %s=(\d+)" % f, line).group(1))
            last[current].append(rec)
    return last


def best(records):
    table = {}
    for r in records:
        if r["pass"] == 0:
            continue
        key = (r["archive"], r["title"])
        table.setdefault(key, []).append(r)
    return table


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "/Volumes/WRBOOT/zimbench.txt"
    data = runs(open(path, errors="replace").read())
    if "old" not in data or "new" not in data:
        sys.exit(f"{path}: need a complete run of both builds (found {sorted(data)})")
    old, new = best(data["old"]), best(data["new"])
    for field, title in (("image_us", "First image decode"),
                         ("retrieve_us", "Article retrieval"),
                         ("screen_us", "Tap to finished first screen")):
        print(f"\n{title}, ms (fastest measured pass)")
        print(f"{'archive':28} {'article':12} {'old':>9} {'new':>9} {'change':>8} {'spread':>7}")
        tot_o = tot_n = 0
        for key in sorted(set(old) & set(new)):
            o = min(r[field] for r in old[key])
            n = min(r[field] for r in new[key])
            if not o or not n:
                continue
            spread = max(r[field] for r in new[key]) / n
            tot_o += o
            tot_n += n
            print(f"{key[0][:28]:28} {key[1][:12]:12} {o/1000:9.1f} {n/1000:9.1f} "
                  f"{(n-o)/o*100:+7.1f}% {spread:6.3f}x")
        if tot_o:
            print(f"{'total':41} {tot_o/1000:9.1f} {tot_n/1000:9.1f} {(tot_n-tot_o)/tot_o*100:+7.1f}%")


if __name__ == "__main__":
    main()
