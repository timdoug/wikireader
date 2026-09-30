#!/usr/bin/env python3
"""Read C33 FDPIC ELF cores and symbolize relocated addresses with binutils."""
import argparse
from dataclasses import dataclass
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

# C33 uses asm-generic Linux numbers. The host may be macOS, whose signal
# numbers differ (Linux SIGBUS=7, macOS SIGBUS=10).
SIGNALS = ("0 HUP INT QUIT ILL TRAP ABRT BUS FPE KILL USR1 SEGV USR2 PIPE ALRM "
           "TERM STKFLT CHLD CONT STOP TSTP TTIN TTOU URG XCPU XFSZ VTALRM "
           "PROF WINCH IO PWR SYS").split()


@dataclass
class Segment:
    kind: int
    offset: int
    address: int
    physical: int
    size: int
    memory: int
    flags: int
    align: int


class ELF:
    def __init__(self, path):
        self.path = Path(path)
        self.data = self.path.read_bytes()
        if len(self.data) < 52 or self.data[:7] != b"\x7fELF\x01\x01\x01":
            raise ValueError(f"{path}: expected little-endian ELF32")
        header = struct.unpack_from("<HHIIIIIHHHHHH", self.data, 16)
        self.kind, machine = header[:2]
        if machine != 107 or not header[6] & 1:
            raise ValueError(f"{path}: expected C33 FDPIC ELF")
        phoff, phsize, count = header[4], header[8], header[9]
        if phsize != 32 or phoff + phsize * count > len(self.data):
            raise ValueError(f"{path}: invalid program header table")
        self.segments = [Segment(*struct.unpack_from("<8I", self.data,
                         phoff + i * phsize)) for i in range(count)]

    def memory(self, address, size):
        for seg in self.segments:
            if seg.kind == 1 and seg.address <= address and \
                    address + size <= seg.address + seg.size:
                offset = seg.offset + address - seg.address
                if offset + size <= len(self.data):
                    return self.data[offset:offset + size]
        raise ValueError(f"core has no saved memory at 0x{address:08x}")

    def notes(self):
        for seg in self.segments:
            if seg.kind != 4:
                continue
            offset, end = seg.offset, seg.offset + seg.size
            if end > len(self.data):
                raise ValueError("truncated ELF notes")
            while offset + 12 <= end:
                namesz, size, kind = struct.unpack_from("<3I", self.data, offset)
                name_start = offset + 12
                start = name_start + ((namesz + 3) & ~3)
                offset = start + ((size + 3) & ~3)
                if offset > end:
                    raise ValueError("invalid ELF note size")
                name = self.data[name_start:name_start + namesz].rstrip(b"\0")
                yield name, kind, self.data[start:start + size]

    def loadmap(self, address):
        if not address:
            return []
        version, count = struct.unpack("<HH", self.memory(address, 4))
        if version != 0 or count > 128:
            raise ValueError("invalid FDPIC load map")
        return [struct.unpack("<3I", self.memory(address + 4 + 12 * i, 12))
                for i in range(count)]


class Symbols:
    def __init__(self, core, maps, directory, exe, addr2line):
        self.core, self.directory = core, directory
        self.addr2line = addr2line
        self.files = {}
        self.maps = []
        self.loads = []
        self.exe = exe
        for line in maps.splitlines():
            match = re.match(r"([0-9a-f]+)-([0-9a-f]+) (\S+) ([0-9a-f]+) "
                             r"\S+ \d+\s*(.*)", line)
            if match:
                start, end, flags, offset, path = match.groups()
                self.maps.append((int(start, 16), int(end, 16), flags,
                                  int(offset, 16), path))

    def elf(self, path):
        if not path.startswith("/") or ".." in Path(path).parts:
            return None
        if path not in self.files:
            saved = self.directory / path.lstrip("/")
            self.files[path] = ELF(saved) if saved.is_file() else None
        return self.files[path]

    def add_loadmap(self, path, address):
        if not path:
            return
        try:
            self.loads.extend((start, start + size, vaddr, path)
                              for start, vaddr, size in self.core.loadmap(address))
        except ValueError as error:
            print(f"Warning: {error}; falling back to /proc maps", file=sys.stderr)

    def relocate(self, address):
        # Executable and interpreter maps from NT_PRSTATUS are authoritative.
        for start, end, vaddr, path in self.loads:
            if start <= address < end:
                return path, vaddr + address - start
        # Shared libraries: translate through file offsets, then PT_LOAD.
        # Each mapping is independent; FDPIC has no single image base.
        for start, end, flags, offset, path in self.maps:
            if start <= address < end:
                elf = self.elf(path)
                if elf:
                    file_offset = offset + address - start
                    for seg in elf.segments:
                        if seg.kind == 1 and seg.offset <= file_offset < seg.offset + seg.size:
                            return path, seg.address + file_offset - seg.offset
                return path or "anonymous", None
        return "unmapped", None

    def describe(self, address):
        path, relative = self.relocate(address)
        elf = self.elf(path)
        if relative is None or elf is None:
            return f"0x{address:08x} {path} (symbols unavailable)"
        result = subprocess.run([self.addr2line, "-f", "-C", "-i", "-e",
                                 str(elf.path), f"0x{relative:x}"],
                                check=True, capture_output=True, text=True)
        lines = result.stdout.strip().splitlines()
        location = " <- ".join(f"{lines[i]} at {lines[i+1]}"
                               for i in range(0, len(lines) - 1, 2))
        return f"0x{address:08x} {path}+0x{relative:x}: {location}"


def analyze(args):
    core = ELF(args.core)
    if core.kind != 4:
        raise ValueError("expected ET_CORE")
    maps_path = args.maps or args.core.with_suffix(".map")
    meta_path = args.core.with_suffix(".txt")
    metadata = {}
    if meta_path.is_file():
        metadata = dict(line.split("=", 1) for line in meta_path.read_text().splitlines()
                        if "=" in line)
    build_path = args.symbols / "build-id"
    if metadata.get("build") and build_path.is_file() and \
            metadata["build"] != build_path.read_text().strip():
        raise ValueError("core and symbol bundle build IDs differ; use the matching build")
    symbols = Symbols(core, maps_path.read_text(), args.symbols,
                      metadata.get("exe", ""), str(args.addr2line))
    threads = [data for name, kind, data in core.notes()
               if name == b"CORE" and kind == 1]
    if not threads:
        raise ValueError("core contains no NT_PRSTATUS")
    executable = symbols.elf(symbols.exe)
    interpreter = ""
    if executable:
        for seg in executable.segments:
            if seg.kind == 3:
                interpreter = executable.data[seg.offset:seg.offset + seg.size].rstrip(b"\0").decode()
    for data in threads:
        if len(data) != 180:
            raise ValueError(f"unexpected C33 NT_PRSTATUS size: {len(data)}")
        exec_map, interp_map = struct.unpack_from("<2I", data, 168)
        symbols.add_loadmap(symbols.exe, exec_map)
        symbols.add_loadmap(interpreter, interp_map)
    truncated = any(seg.kind == 1 and seg.offset + seg.size > len(core.data)
                    for seg in core.segments)
    print(f"C33 FDPIC core: {symbols.exe or 'unknown executable'}, {len(core.data)} bytes, "
          f"{'truncated' if truncated else metadata.get('status', 'complete')}")
    for data in threads:
        sig = struct.unpack_from("<h", data, 12)[0]
        pid = struct.unpack_from("<i", data, 24)[0]
        regs = struct.unpack_from("<24I", data, 72)
        print(f"PID {pid}, SIG{SIGNALS[sig] if 0 < sig < len(SIGNALS) else sig}")
        for i in range(0, 16, 4):
            print("  " + "  ".join(f"r{j}={regs[j]:08x}" for j in range(i, i + 4)))
        print(f"  alr={regs[16]:08x} ahr={regs[17]:08x} sp={regs[18]:08x} "
              f"psr={regs[22]:08x} pc={regs[23]:08x}")
        print("  PC: " + symbols.describe(regs[23]))
        if args.stack:
            print("  Stack words pointing into executable mappings (candidates, not a backtrace):")
            for offset in range(0, args.stack * 4, 4):
                try:
                    word, = struct.unpack("<I", core.memory(regs[18] + offset, 4))
                except ValueError:
                    break
                if any(start <= word < end and "x" in flags
                       for start, end, flags, _, _ in symbols.maps):
                    print(f"    sp+0x{offset:x}: {symbols.describe(word)}")
    for address in args.address:
        print("Address: " + symbols.describe(address))


def main():
    root = Path(__file__).resolve().parents[2]
    default_tool = root / "host-tools/toolchain-c33/work/install/bin/c33-epson-elf-addr2line"
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("core", type=Path)
    parser.add_argument("--maps", type=Path, help="defaults to CORE basename.map")
    parser.add_argument("--symbols", type=Path, default=root / "linux/artifacts/symbols")
    parser.add_argument("--addr2line", type=Path, default=default_tool)
    parser.add_argument("--address", type=lambda value: int(value, 0), action="append", default=[])
    parser.add_argument("--stack", type=int, default=0, metavar="WORDS")
    args = parser.parse_args()
    if not 0 <= args.stack <= 4096:
        parser.error("--stack must be between 0 and 4096")
    if not args.addr2line.is_file() and not shutil.which(str(args.addr2line)):
        parser.error("C33 addr2line not found; build the host binutils or pass --addr2line")
    try:
        analyze(args)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"core: {error}\n")


if __name__ == "__main__":
    main()
