#!/usr/bin/env python3
"""Create a disposable FAT32 emulator card holding the model and the app.

Built in memory and written once, so nothing is mounted: a stale mount of a
previous image is the classic way to install a card that silently still has
the old files on it.
"""
import argparse
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# One sector per cluster is what diskutil gives a small FAT32 volume, and it
# is also the slowest thing the SD driver can be asked to do: NuttX and
# grifo both clip a multi-sector read to the cluster, so it pays a command
# and a response for every 512 bytes. Eight sectors is the boot volume's
# geometry after the same fix.
SECTORS_PER_CLUSTER = 8

# FatFs types a volume by cluster count with `if (nclst <= MAX_FAT16) fmt =
# FS_FAT16`, and MAX_FAT16 is 0xFFF5 -- so a volume with exactly 65525
# clusters is FAT16, not FAT32, and is then rejected for having a zero
# BPB_RootEntCnt. The usual "65525 clusters" figure is the FAT16 maximum;
# the FAT32 minimum is one more than that.
MIN_FAT32_CLUSTERS = 65526

# init.app drops any menu entry whose icon will not load, so the card needs
# one whatever it looks like. The format is 64 rows of 8 bytes, most
# significant bit first, 1 = black -- small enough to draw here rather than
# carry an XPM and a conversion step for a disposable test card.
LLAMA_ICON = """
................................
.......#.#......................
.......#.#......................
......#####.....................
......#####.....................
......#####.....................
.......####.....................
.......####.....................
.......####.....................
........####....................
........####....................
........#####...................
........######..................
........#######.................
.......##########...............
......##############............
.....#################..........
....####################........
....#####################.......
....######################......
....######################...##.
....######################..###.
....#####################..###..
....####################...##...
....###################.........
....###################.........
....##...####....####...........
....##...####....####...........
....##...####....####...........
....##...####....####...........
....##...####....####...........
................................
"""


def icon_bytes(art=LLAMA_ICON, size=64):
    rows = [r for r in art.strip("\n").split("\n")]
    if len(rows) * 2 != size or any(len(r) * 2 != size for r in rows):
        raise ValueError("icon art must be half the icon size in each axis")
    out = bytearray()
    for row in rows:
        # Double each row and each pixel: the art is drawn at 32x32 and the
        # icon is 64x64.
        bits = "".join(c * 2 for c in row)
        packed = bytes(
            sum((bits[x + b] != ".") << (7 - b) for b in range(8))
            for x in range(0, size, 8)
        )
        out += packed * 2
    if len(out) != size * size // 8:
        raise ValueError("icon came out the wrong size")
    return bytes(out)


def make_card(output, model, vocab, arguments,
              app=ROOT / "llama/llama.app", force=False):
    files = {
        "KERNEL.ELF": (ROOT / "samo-lib/grifo/grifo.elf").read_bytes(),
        "INIT.APP": (
            ROOT / "samo-lib/grifo/applications/init/init.app"
        ).read_bytes(),
        "LLAMA.APP": app.read_bytes(),
        "LLAMA.ICO": icon_bytes(),
        "MODEL.WRL": model.read_bytes(),
        "TOK.BIN": vocab.read_bytes(),
        # <icon> : <command>; init.app ignores an entry whose icon is
        # missing, and chains straight into the command when there is
        # exactly one.
        "INIT.INI": ("llama.ico : llama.app " + arguments.strip()
                     + "\n").encode(),
    }
    if files["MODEL.WRL"][:4] != b"WRL2":
        raise ValueError(f"{model} is not a WRL2 weight file; run tools/convert.py")

    spc = SECTORS_PER_CLUSTER
    part, reserved, fat_sectors = 2048, 32, 2048
    total = sum(
        max(1, (len(d) + spc * 512 - 1) // (spc * 512)) for d in files.values()
    )
    # Root directory, plus slack, plus the minimum FAT32 cluster count.
    clusters = max(MIN_FAT32_CLUSTERS, total + 16)
    sectors = reserved + 2 * fat_sectors + clusters * spc
    data_sector = part + reserved + 2 * fat_sectors

    fat = bytearray(fat_sectors * 512)
    struct.pack_into("<III", fat, 0, 0x0FFFFFF8, 0xFFFFFFFF, 0x0FFFFFFF)
    next_cluster = 3
    payloads = []

    def allocate(data):
        nonlocal next_cluster
        count = max(1, (len(data) + spc * 512 - 1) // (spc * 512))
        start = next_cluster
        next_cluster += count
        if next_cluster > clusters + 2:
            raise ValueError("Files do not fit the card")
        for n in range(start, next_cluster):
            struct.pack_into(
                "<I", fat, n * 4,
                0x0FFFFFFF if n + 1 == next_cluster else n + 1,
            )
        payloads.append((start, data))
        return start

    def entry(name, cluster, size):
        base, _, ext = name.upper().partition(".")
        record = bytearray(32)
        record[:11] = (base.ljust(8) + ext.ljust(3)).encode("ascii")
        record[11] = 0x20
        struct.pack_into("<H", record, 20, cluster >> 16)
        struct.pack_into("<HI", record, 26, cluster & 65535, size)
        return record

    root = bytearray()
    for name, data in files.items():
        root += entry(name, allocate(data), len(data))
    payloads.append((2, root))

    mbr = bytearray(512)
    mbr[446:462] = struct.pack(
        "<B3sB3sII", 0, b"\xfe\xff\xff", 0x0C, b"\xfe\xff\xff", part, sectors
    )
    mbr[510:] = b"\x55\xaa"
    boot = bytearray(512)
    boot[:11] = b"\xeb\x58\x90WRLLAMA "
    struct.pack_into(
        "<HBHBHHBHHHII", boot, 11,
        512, spc, reserved, 2, 0, 0, 0xF8, 0, 63, 255, part, sectors,
    )
    struct.pack_into("<IHHIHH", boot, 36, fat_sectors, 0, 0, 2, 1, 6)
    boot[64] = 0x80
    boot[66] = 0x29
    struct.pack_into("<I", boot, 67, 0x4C4C414D)
    boot[71:90] = b"WRLLAMA    FAT32   "
    boot[510:] = b"\x55\xaa"
    info = bytearray(512)
    struct.pack_into("<I", info, 0, 0x41615252)
    struct.pack_into(
        "<III", info, 484, 0x61417272,
        clusters - (next_cluster - 2), next_cluster - 1,
    )
    struct.pack_into("<I", info, 508, 0xAA550000)

    # Exclusive creation by default: an image path is one slip away from a
    # device node, and this writes a partition table to sector zero.
    # --force relaxes that only for an ordinary file, which is what the
    # build-and-run loop actually wants to replace.
    if force and output.exists():
        if not output.is_file():
            raise SystemExit(
                f"{output} is not a regular file; refusing to overwrite it "
                "even with --force"
            )
        output.unlink()
    try:
        image = output.open("xb")
    except FileExistsError:
        raise SystemExit(
            f"{output} already exists. Pass --force to replace it, or "
            f"remove it first."
        ) from None
    with image:
        image.truncate((part + sectors) * 512)

        def write(sector, data):
            image.seek(sector * 512)
            image.write(data)

        write(0, mbr)
        for offset in (0, 6):
            write(part + offset, boot)
            write(part + offset + 1, info)
        write(part + reserved, fat)
        write(part + reserved + fat_sectors, fat)
        for cluster, data in payloads:
            write(data_sector + (cluster - 2) * spc, data)
    print(output)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("model", type=Path, help="a .wrl from tools/convert.py")
    parser.add_argument("vocab", type=Path, help="llama2.c tokenizer.bin")
    # argparse refuses a lone value that looks like an option, so
    # `--args "-v"` fails while `--args "-v -n 64"` works. Use the equals
    # form, which is never ambiguous.
    parser.add_argument(
        "--args", default="", metavar="'-v -n 64'",
        help="application arguments, baked into init.ini on the card and "
             "passed to llama.app. Write them as --args='-v'. These have "
             "nothing to do with wremu's own -n, which is an instruction "
             "budget; the app's -n is a token count, and omitting it "
             "generates until the story ends.",
    )
    parser.add_argument("--app", type=Path, default=ROOT / "llama/llama.app")
    parser.add_argument(
        "-f", "--force", action="store_true",
        help="replace the output if it is an ordinary file",
    )
    args = parser.parse_args()
    make_card(args.output, args.model, args.vocab, args.args, args.app,
              args.force)
