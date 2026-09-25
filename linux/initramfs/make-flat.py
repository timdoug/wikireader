#!/usr/bin/env python3
"""Convert a linked C33 ELF image into a relocatable version-4 bFLT file."""

import argparse
import struct
from pathlib import Path


C33_32 = 1
C33_H = 9
C33_M = 10
C33_L = 11
C33_DH = 12
C33_DL = 13
C33_PC_RELATIVE = {6, 7, 8, 24, 25, 26, 27, 28}
C33_SPLIT_RELOC = 0x80000000
FLAT_FLAG_RAM = 0x0001
SHT_RELA = 4


class Section:
    def __init__(self, index, name, fields, image):
        self.index = index
        self.name = name
        (self.name_offset, self.type, self.flags, self.address, self.offset,
         self.size, self.link, self.info, self.alignment,
         self.entry_size) = fields
        self.contents = image[self.offset:self.offset + self.size]


def c_string(table, offset):
    end = table.find(b"\0", offset)
    if end < 0:
        raise ValueError("unterminated ELF section name")
    return table[offset:end].decode("ascii")


def read_elf(path):
    image = path.read_bytes()
    header_format = "<16sHHIIIIIHHHHHH"
    if len(image) < struct.calcsize(header_format):
        raise ValueError("ELF file is truncated")
    header = struct.unpack_from(header_format, image)
    ident = header[0]
    if ident[:4] != b"\x7fELF" or ident[4:6] != b"\x01\x01":
        raise ValueError("expected a little-endian ELF32 file")

    entry = header[4]
    section_offset = header[6]
    section_entry_size = header[11]
    section_count = header[12]
    string_index = header[13]
    section_format = "<IIIIIIIIII"
    if section_entry_size != struct.calcsize(section_format):
        raise ValueError("unexpected ELF section-header size")

    raw_sections = []
    for index in range(section_count):
        offset = section_offset + index * section_entry_size
        raw_sections.append(struct.unpack_from(section_format, image, offset))
    string_fields = raw_sections[string_index]
    strings = image[string_fields[4]:string_fields[4] + string_fields[5]]
    sections = [
        Section(index, c_string(strings, fields[0]), fields, image)
        for index, fields in enumerate(raw_sections)
    ]
    return entry, sections


def section_by_name(sections, name, required=True):
    matches = [section for section in sections if section.name == name]
    if len(matches) > 1:
        raise ValueError(f"ELF contains multiple {name} sections")
    if not matches:
        if required:
            raise ValueError(f"ELF has no {name} section")
        return None
    return matches[0]


def make_segments(sections):
    text = section_by_name(sections, ".text")
    data = section_by_name(sections, ".data", required=False)
    bss = section_by_name(sections, ".bss", required=False)
    if text.address < 4:
        raise ValueError(".text must leave offset zero unavailable for symbols")

    data_address = data.address if data else (bss.address if bss else
                                               text.address + text.size)
    text_end = text.address + text.size
    if data_address < text_end:
        raise ValueError(".data overlaps .text")
    text_image = bytes(text.address) + text.contents + bytes(data_address - text_end)

    data_image = data.contents if data else b""
    data_end = data_address + len(data_image)
    bss_end = bss.address + bss.size if bss else data_end
    if bss_end < data_end:
        raise ValueError(".bss overlaps .data")
    return text_image, data_image, bss_end - data_end


def check_data_offset(sections, address, data_address, data_end):
    """Check a %r15-relative (doff_hi/doff_lo) access lands in the data segment.

    The linker has already resolved these against __dp, the start of .data,
    and the kernel points %r15 at wherever it places that segment, so they
    need no load-time relocation.  They are only right if the target really
    is in .data or .bss: a variable declared writable but defined read-only
    would be reached at a wrong address.
    """
    text = section_by_name(sections, ".text")
    offset = address - text.address
    high, low = struct.unpack_from("<HH", text.contents, offset)
    target = data_address + (((high & 0x1fff) << 13) | (low & 0x1fff))
    if not data_address <= target < data_end:
        raise ValueError(f"%r15-relative access at 0x{address:x} reaches "
                         f"0x{target:x}, outside the data segment")


def make_relocations(sections, image_size, data_address, data_end):
    records = []
    loadable = {section.index for section in sections
                if section.name in (".text", ".data")}
    for section in sections:
        if section.type != SHT_RELA or section.info not in loadable:
            continue
        if section.entry_size != 12 or section.size % section.entry_size:
            raise ValueError(f"malformed relocation section {section.name}")
        for offset in range(0, section.size, section.entry_size):
            address, info, addend = struct.unpack_from("<IIi", section.contents,
                                                        offset)
            records.append((address, info & 0xff, info >> 8, addend))

    records.sort()
    relocations = []
    index = 0
    while index < len(records):
        address, kind, symbol, addend = records[index]
        if kind == C33_H:
            triple = records[index:index + 3]
            expected = [
                (address, C33_H, symbol, addend),
                (address + 2, C33_M, symbol, addend),
                (address + 4, C33_L, symbol, addend),
            ]
            if triple != expected:
                raise ValueError(f"incomplete C33 H/M/L relocation at 0x{address:x}")
            relocations.append(C33_SPLIT_RELOC | address)
            index += 3
            continue
        if kind in (C33_M, C33_L):
            raise ValueError(f"orphaned C33 split relocation at 0x{address:x}")
        if kind == C33_DH:
            pair = records[index:index + 2]
            if (len(pair) != 2 or pair[1][:3] != (address + 2, C33_DL, symbol)
                    or pair[1][3] != addend):
                raise ValueError(f"incomplete doff relocation at 0x{address:x}")
            check_data_offset(sections, address, data_address, data_end)
            index += 2
            continue
        if kind == C33_DL:
            raise ValueError(f"orphaned doff_lo relocation at 0x{address:x}")
        if kind == C33_32:
            relocations.append(address)
        elif kind != 0 and kind not in C33_PC_RELATIVE:
            raise ValueError(f"unsupported C33 relocation {kind} at 0x{address:x}")
        index += 1

    for relocation in relocations:
        address = relocation & ~C33_SPLIT_RELOC
        width = 6 if relocation & C33_SPLIT_RELOC else 4
        if address + width > image_size:
            raise ValueError(f"relocation at 0x{address:x} is outside the image")
    return relocations


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("elf", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--shared-text", action="store_true",
                        help="fail unless the text needs no relocation, so "
                             "the kernel can share it between processes")
    args = parser.parse_args()

    entry, sections = read_elf(args.elf)
    text, data, bss_size = make_segments(sections)
    relocations = make_relocations(sections, len(text) + len(data),
                                   len(text), len(text) + len(data) + bss_size)
    text_relocations = [relocation for relocation in relocations
                        if relocation & ~C33_SPLIT_RELOC < len(text)]
    if args.shared_text and text_relocations:
        shown = ", ".join(f"0x{relocation & ~C33_SPLIT_RELOC:x}"
                          for relocation in text_relocations[:8])
        raise SystemExit(f"{args.elf}: {len(text_relocations)} relocations in "
                         f"text, which -msep-data code should not have "
                         f"(first at {shown})")
    # Without FLAT_FLAG_RAM binfmt_flat maps the text read-only from the file,
    # and a read-only private mapping is shared by every process running
    # it.  Relocations in text need a private, writable copy instead.
    flags = FLAT_FLAG_RAM if text_relocations else 0
    header_size = 64
    data_start = header_size + len(text)
    data_end = data_start + len(data)
    fields = [
        4,              # format revision
        header_size + entry,
        data_start,
        data_end,
        data_end + bss_size,
        16 * 1024,      # stack
        data_end,
        len(relocations),
        flags,
        0,              # build date
        0, 0, 0, 0, 0,
    ]
    relocation_table = b"".join(struct.pack(">I", value)
                                  for value in relocations)
    args.output.write_bytes(struct.pack(">4s15I", b"bFLT", *fields) +
                            text + data + relocation_table)
    print(f"bFLT: {len(text)} text, {len(data)} data, {bss_size} bss, "
          f"{len(relocations)} relocations, "
          f"{'private' if text_relocations else 'shared'} text")


if __name__ == "__main__":
    main()
