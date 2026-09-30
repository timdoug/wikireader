#!/usr/bin/env python3
"""Make an old-style header fixture to exercise linear FDPIC FDE lookup."""
from pathlib import Path
import struct
import sys

source, destination = map(Path, sys.argv[1:])
data = bytearray(source.read_bytes())
eh = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)
for i in range(eh[9]):
    ph = struct.unpack_from("<8I", data, eh[4] + i * eh[8])
    if ph[0] == 0x6474e550:  # PT_GNU_EH_FRAME
        if data[ph[1]:ph[1] + 4] != b"\x01\x1b\x03\x3b":
            raise SystemExit("Expected a linker-generated table")
        # Keep the frame pointer and mappings; omit count and search table
        # as an older eight-byte header did. Trailing bytes are ignored.
        # The header still lives apart from the writable frame records.
        data[ph[1] + 2:ph[1] + 4] = b"\xff\xff"
        destination.write_bytes(data)
        break
else:
    raise SystemExit("Missing PT_GNU_EH_FRAME")
