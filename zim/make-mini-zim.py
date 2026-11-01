#!/usr/bin/env python3
"""Write a small ZIM holding a few articles of a big one, with their images.

  zim/make-mini-zim.py SOURCE.zim OUTPUT.zim Title [Title ...]

For running the reader in the emulator on pages from an archive too large for
a card image (the 124 GB English Wikipedia): the articles' HTML and every
image they reference are copied unchanged.  Each article gets a cluster of
its own, Zstandard-compressed like Kiwix's, and the images share one stored
uncompressed, as Kiwix stores them.  Real clusters hold some 85 articles, so
cluster decoding costs less here than on the real archive; conversion,
wrapping, the preview and the images run as on the device.
Name the output after the source (wikipedia_en_all_...) so zimbench.lst
entries match it.
"""
import compression.zstd
import hashlib
import re
import struct
import subprocess
import sys
import urllib.parse
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ZIMDUMP = ROOT / "host-tools/zim-reader/zimdump"


def blob(source, path):
    run = subprocess.run([str(ZIMDUMP), str(source), "blob", "C", path],
                         capture_output=True)
    return run.stdout if run.returncode == 0 and run.stdout else None


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    source, output, titles = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3:]
    entries, seen = [], set()
    for title in titles:
        path = title.replace(" ", "_")
        html = blob(source, path)
        if html is None:
            sys.exit(f"{title}: not in {source}")
        entries.append(("C", path, title, 0, ("article", path), html))
        for src in re.findall(rb'<img[^>]+src="([^"]+)"', html):
            src = src.decode()
            if src.startswith("./"):
                src = src[2:]
            # The reader undoes one layer of escapes before the lookup.
            name = urllib.parse.unquote(src)
            if name in seen:
                continue
            data = blob(source, name)
            if data is None:
                continue
            seen.add(name)
            entries.append(("C", name, "", 1, ("images",), data))
        print(f"{title}: {len(html)} bytes of HTML, {len(seen)} images so far")
    entries.append(("M", "Title", "Title", 2, ("images",),
                    b"Mini archive for emulator runs"))
    entries.append(("X", "listing/titleOrdered/v1", "", 2, ("images",), b""))
    entries.sort(key=lambda e: (e[0], e[1]))
    title_order = sorted((i for i, e in enumerate(entries)
                          if e[0] == "C" and e[2]), key=lambda i: entries[i][2])
    entries = [e if e[0] != "X" else e[:5] + (struct.pack(
        "<" + "I" * len(title_order), *title_order),) for e in entries]
    # Clusters in order of first use; each entry's blob index within its own.
    clusters = {}
    placed = []
    for e in entries:
        members = clusters.setdefault(e[4], [])
        placed.append((list(clusters).index(e[4]), len(members)))
        members.append(e[5])
    mime = b"text/html\0image/webp\0text/plain\0\0"
    pp = 80 + len(mime)
    tp = pp + 8 * len(entries)
    dirs = bytearray()
    pointers = []
    cp = tp + 4 * len(entries)
    dp = cp + 8 * len(clusters)
    for (ns, path, title, kind, key, _), (cluster, index) in zip(entries, placed):
        pointers.append(dp + len(dirs))
        dirs += struct.pack("<HBBIII", kind, 0, ord(ns), 0, cluster, index)
        dirs += path.encode() + b"\0" + title.encode() + b"\0"
    body = bytearray()
    cluster_pointers = []
    for key, members in clusters.items():
        offsets = [4 * (len(members) + 1)]
        for b in members:
            offsets.append(offsets[-1] + len(b))
        payload = struct.pack("<" + "I" * len(offsets), *offsets) + b"".join(members)
        cluster_pointers.append(dp + len(dirs) + len(body))
        if key[0] == "article":
            body += b"\x05" + compression.zstd.compress(payload, level=19)
        else:
            body += b"\x01" + payload
    end = dp + len(dirs) + len(body)
    header = struct.pack("<IHH16sIIQQQQIIQ", 72173914, 6, 1, b"mini-archive-fix",
                         len(entries), len(clusters), pp, tp, cp, 80, 0,
                         0xffffffff, end)
    data = (header + mime + struct.pack("<" + "Q" * len(pointers), *pointers) +
            struct.pack("<" + "I" * len(entries), *range(len(entries))) +
            struct.pack("<" + "Q" * len(clusters), *cluster_pointers) + dirs + body)
    output.write_bytes(data + hashlib.md5(data).digest())
    print(f"{output}: {len(data) + 16} bytes, {len(entries)} entries")


if __name__ == "__main__":
    main()
