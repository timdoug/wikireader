#!/usr/bin/env python3
"""Convert a linked C33 ELF image into a relocatable version-4 bFLT file."""

import argparse
import struct
from pathlib import Path


C33_32 = 1
C33_H = 9
C33_M = 10
C33_L = 11
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


def make_relocations(sections, image_size):
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
    args = parser.parse_args()

    entry, sections = read_elf(args.elf)
    text, data, bss_size = make_segments(sections)
    relocations = make_relocations(sections, len(text) + len(data))
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
        FLAT_FLAG_RAM,  # text relocations require a writable RAM image
        0,              # build date
        0, 0, 0, 0, 0,
    ]
    relocation_table = b"".join(struct.pack(">I", value)
                                  for value in relocations)
    args.output.write_bytes(struct.pack(">4s15I", b"bFLT", *fields) +
                            text + data + relocation_table)
    print(f"bFLT: {len(text)} text, {len(data)} data, {bss_size} bss, "
          f"{len(relocations)} relocations")


if __name__ == "__main__":
    main()
