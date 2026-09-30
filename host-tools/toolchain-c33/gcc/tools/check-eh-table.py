#!/usr/bin/env python3
"""Check C33 FDPIC unwind tables before installing the dynamic runtime."""
from pathlib import Path
import struct
import sys


def check(path):
    data = path.read_bytes()
    if data[:7] != b"\x7fELF\x01\x01\x01":
        raise ValueError("expected little-endian ELF32")
    eh = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)
    if not eh[6] & 1:
        raise ValueError("expected C33 FDPIC")
    loads = [struct.unpack_from("<8I", data, eh[4] + i * eh[8])
             for i in range(eh[9])]
    sections = [struct.unpack_from("<10I", data, eh[5] + i * eh[10])
                for i in range(eh[11])]
    names = sections[eh[12]]
    strings = data[names[4]:names[4] + names[5]]
    named = {strings[s[0]:strings.index(0, s[0])].decode(): s for s in sections}
    hdr, frames = named[".eh_frame_hdr"], named[".eh_frame"]
    contents = data[hdr[4]:hdr[4] + hdr[5]]
    if len(contents) < 12 or contents[:4] != b"\x01\x1b\x03\x3b":
        raise ValueError("missing linker-generated binary search table")
    pointer, count = struct.unpack_from("<iI", contents, 4)
    if hdr[3] + 4 + pointer != frames[3] or not count or len(contents) != 12 + count * 8:
        raise ValueError("bad frame pointer, count or table size")
    if not any(p[0] == 0x6474e550 and p[2] == hdr[3] and p[5] == hdr[5] for p in loads):
        raise ValueError("PT_GNU_EH_FRAME does not cover the table")
    if not any(p[0] == 1 and not p[6] & 2 and p[2] <= hdr[3] and
               hdr[3] + hdr[5] <= p[2] + p[4] for p in loads):
        raise ValueError("search table is not in a shared read-only segment")
    previous = -1
    for i in range(count):
        initial, fde = struct.unpack_from("<ii", contents, 12 + i * 8)
        initial = (hdr[3] + initial) & 0xffffffff
        fde = (hdr[3] + fde) & 0xffffffff
        if initial < previous or not any(p[0] == 1 and p[6] & 1 and
                                         p[2] <= initial < p[2] + p[5] for p in loads):
            raise ValueError("unsorted table or code address outside executable segments")
        previous = initial
        offset = fde - frames[3]
        if not 0 <= offset <= frames[5] - 16:
            raise ValueError("FDE outside frame section")
        length, cie, pc, span = struct.unpack_from("<4I", data, frames[4] + offset)
        if length < 12 or offset + 4 + length > frames[5] or not cie or pc != initial or not span:
            raise ValueError("table entry disagrees with its absolute C33 FDE")
    # Neither the regular dynamic relocations nor executable self-relocation
    # may rewrite these link-time differences.
    for s in sections:
        if s[1] == 4:  # SHT_RELA
            for offset in range(s[4], s[4] + s[5], s[9]):
                address = struct.unpack_from("<I", data, offset)[0]
                if hdr[3] <= address < hdr[3] + hdr[5]:
                    raise ValueError("runtime relocation targets the search table")
    fixups = named.get(".rofixup")
    if fixups:
        for offset in range(fixups[4], fixups[4] + fixups[5], 4):
            address = struct.unpack_from("<I", data, offset)[0]
            if hdr[3] <= address < hdr[3] + hdr[5]:
                raise ValueError("self-relocation targets the search table")
    print(f"{path.name}: {count} FDEs, {hdr[5]} bytes, link-time table passed")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit("Usage: check-eh-table.py ELF...")
    for argument in sys.argv[1:]:
        try:
            check(Path(argument))
        except (ValueError, KeyError, struct.error) as error:
            raise SystemExit(f"{argument}: {error}")
