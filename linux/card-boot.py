#!/usr/bin/env python3
"""Boot wremu on a copy of a device card's WRBOOT partition, through the real
chain: card MBR flash -> Grifo -> init.app -> the application.  Take the
partition with `sudo dd if=/dev/rdiskNs1 of=/tmp/wrboot.img bs=1m`; CARD
names another copy.  init.ini is replaced by INI (default the Linux line
with ARGS), so the launcher starts it without a tap; REPLACE=name=path swaps
a file's contents in place (same cluster chain, must fit)."""
import os, shlex, struct, subprocess, sys, tempfile
from pathlib import Path
root = Path(__file__).resolve().parent.parent
part = bytearray(Path(os.environ.get('CARD', '/tmp/wrboot.img')).read_bytes())
bps, spc, rsv, nfats = struct.unpack_from('<HBHB', part, 11)
fatsz = struct.unpack_from('<I', part, 36)[0]
rootc = struct.unpack_from('<I', part, 44)[0]
fat_off = rsv * bps
data = rsv + nfats * fatsz
csize = spc * bps
def ent(c): return struct.unpack_from('<I', part, fat_off + c * 4)[0] & 0x0fffffff
def chain(c):
    out = []
    while 2 <= c < 0x0ffffff8:
        out.append(c); c = ent(c)
    return out
def coff(c): return (data + (c - 2) * spc) * bps
rootchain = chain(rootc)
def entries():
    for c in rootchain:
        for i in range(0, csize, 32):
            o = coff(c) + i
            e = part[o:o+32]
            if e[0] in (0, 0xe5) or e[11] == 0x0f:
                continue
            yield o, e[:11].decode('latin1')
def replace(name83, content):
    for o, n in entries():
        if n == name83:
            start = struct.unpack_from('<H', part, o+20)[0] << 16 | struct.unpack_from('<H', part, o+26)[0]
            ch = chain(start)
            assert len(content) <= len(ch) * csize, (name83, len(content), len(ch) * csize)
            for k, c in enumerate(ch):
                piece = content[k*csize:(k+1)*csize]
                part[coff(c):coff(c)+len(piece)] = piece
            struct.pack_into('<I', part, o+28, len(content))
            return
    raise SystemExit('no ' + name83)
def n83(name):
    stem, ext = name.upper().split('.')
    return stem.ljust(8) + ext.ljust(3)
replace(n83('init.ini'), os.environ.get('INI', 'linux.ico : linux.app ' + os.environ.get('ARGS', '')).encode() + b'\n')
for item in filter(None, os.environ.get('REPLACE', '').split(',')):
    name, path = item.split('=')
    replace(n83(name), Path(path).read_bytes())
out = Path(tempfile.mkdtemp(prefix='card-boot.'))
mbr = bytearray(512)
mbr[450] = 0x0c
start = 2048
struct.pack_into('<II', mbr, 454, start, len(part) // 512)
mbr[510:] = b'\x55\xaa'
with open(out / 'card.img', 'wb') as f:
    f.write(mbr); f.seek(start * 512); f.write(part)
subprocess.run([sys.executable, str(root / 'samo-lib/mbr/make-flash.py'), str(out / 'flash.rom')], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
cmd = [str(root / 'emulator/wremu'), '-n', os.environ.get('N', '1000000000'), *shlex.split(os.environ.get('EXTRA', '')), '-c', str(out / 'card.img'), '-e', str(out / 'flash.rom')]
with open(out / 'boot.log', 'w') as log:
    subprocess.run(cmd, cwd=out, stdout=log, stderr=subprocess.STDOUT, env={**os.environ, 'WREMU_UART_TRACE': ''})
print(out)
