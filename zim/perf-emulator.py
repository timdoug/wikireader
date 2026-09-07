#!/usr/bin/env python3
"""Profile an installed ZIM reader on a scratch card, using its matching map."""
import argparse
import os
from pathlib import Path
import re
import subprocess


def main():
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("output", type=Path, help="new directory for log, profile and screen")
    parser.add_argument("--map", type=Path, default=root / "zim/zim.map")
    parser.add_argument("--phase", choices=("startup", "article", "image"), default="article")
    parser.add_argument("--word", default="CAT")
    parser.add_argument("--board", choices=("7", "8"), default="7")
    args = parser.parse_args()
    symbols = dict((name, address) for address, name in re.findall(
        r"^\s+(0x[0-9a-f]+)\s+(\w+)\s*$", args.map.read_text(), re.MULTILINE))
    # grifo_main is also init.app's entry at the same address. wikilib_init
    # gets inlined, so neither is a valid reader-start milestone.
    start, end, limit = {
        "startup": ("wikilib_run", "render_search_result_with_pcf", 180000000),
        "article": ("retrieve_article", "render_article_with_pcf", 620000000),
        "image": ("zim_image_decoder_create", "zim_image_decoder_destroy", 800000000),
    }[args.phase]
    args.output.mkdir(parents=True, exist_ok=False)
    cmd = [str(root / "emulator/wremu"), "-R", "-e", str(root / "samo-lib/mbr/flash.rom"),
           "-c", str(args.image.resolve()), "-T", "40,36,100000000",
           "-Y", symbols[start] + "," + symbols[end], "-X", symbols[end] + ",phase_end",
           "-F", "prof.txt", "-n", str(limit)]
    if args.phase != "startup":
        cmd += ["-K", "300000000," + args.word, "-T", "30,40,500000000"]
    env = dict(os.environ, WREMU_BOARD_REV=args.board)
    with (args.output / "run.log").open("w") as log:
        subprocess.run(cmd, cwd=args.output, env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
    output = (args.output / "run.log").read_text(errors="replace")
    match = re.search(r"^--- window .*: (\d+) instructions, ([\d.]+) ms,.*$", output, re.MULTILINE)
    ended = re.search(r"^--- probe phase_end\s+[1-9]\d* hits", output, re.MULTILINE)
    if not match or not int(match[1]) or not ended or re.search(r"^fault:|panic", output, re.MULTILINE | re.IGNORECASE):
        raise SystemExit("Failed to reach the requested phase; inspect " + str(args.output / "run.log"))
    print(match[0])
    print("Artifacts:", args.output.resolve())


if __name__ == "__main__":
    main()
