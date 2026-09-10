#!/usr/bin/env python3
"""Apply canonical article-title precedence to a completed Sparrow index.

No source claims change. This lets an older, already-running full import gain
the new alias policy without restarting it. Metadata omitted by the original
builder remains omitted; this cannot recover an oversized title or an alias.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import sqlite3
import struct
import tempfile
import zlib

MISSING = 0xffffffff


def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as f:
        for chunk in iter(lambda: f.read(1024*1024), b''): digest.update(chunk)
    return digest.hexdigest()


def normalize(text):
    text = text.translate(str.maketrans('ABCDEFGHIJKLMNOPQRSTUVWXYZ_', 'abcdefghijklmnopqrstuvwxyz '))
    import re
    return re.sub(r'[ \t\r\n]+', ' ', text).strip(' ')


def blocks(path, table, count, maximum, payload_start):
    size = path.stat().st_size
    with path.open('rb') as directory, path.open('rb') as data:
        directory.seek(table)
        for i in range(count):
            raw = directory.read(16)
            if len(raw) != 16: raise ValueError('short directory')
            offset, length, crc = struct.unpack('<QII', raw)
            if offset < payload_start or offset % 512 or length > maximum or offset+length > size:
                raise ValueError('invalid block bounds')
            data.seek(offset); block = data.read(length)
            if zlib.crc32(block) != crc: raise ValueError('block checksum mismatch')
            yield i, offset, block


def upgrade(source, output):
    source = source.resolve(); output = output.resolve()
    if output.exists() or Path(str(output)+'.json').exists(): raise ValueError('output already exists')
    report = json.loads(Path(str(source)+'.json').read_text())
    source_sha = sha(source)
    if source_sha != report['sha256']: raise ValueError('source checksum mismatch')
    with source.open('rb') as f: header = f.read(128)
    magic, version, length, size, buckets, entities, bp, ep = struct.unpack_from('<8sIIQIIQQ',header)
    if (magic != b'SPRWQA1\0' or version != 1 or length != 128 or size != source.stat().st_size
            or zlib.crc32(header[:80]) != struct.unpack_from('<I',header,80)[0]
            or not buckets or buckets & (buckets-1) or bp != 512
            or ep < bp+buckets*16 or ep+entities*16 > size):
        raise ValueError('invalid index header')
    output.parent.mkdir(parents=True, exist_ok=True)
    # Budget for a normal copy and the title table; do not depend on APFS.
    if shutil.disk_usage(output.parent).free < size + 24*1024**3:
        raise OSError('insufficient room for title upgrade and 24 GiB reserve')
    changed = disambiguated = ambiguous = 0
    with tempfile.TemporaryDirectory(prefix='title-index-', dir=output.parent) as tmp:
        tmp = Path(tmp)
        with sqlite3.connect(tmp/'titles.db') as db:
            db.execute('PRAGMA journal_mode=OFF'); db.execute('PRAGMA cache_size=-32768')
            db.execute('CREATE TABLE title(key TEXT PRIMARY KEY, id INTEGER) WITHOUT ROWID')
            for i, _, data in blocks(source, ep, entities, 65536, ep+entities*16):
                if len(data) < 12: raise ValueError('short entity')
                ln, tn = struct.unpack_from('<HH',data,4)
                if ln > 255 or tn > 255 or 12+ln+tn > len(data): raise ValueError('invalid metadata')
                key = normalize(data[12+ln:12+ln+tn].decode('utf-8'))
                if key and len(key.encode()) <= 127 and '\0' not in key:
                    db.execute('INSERT INTO title VALUES(?,?) ON CONFLICT(key) DO UPDATE SET id=?',
                               (key,i,MISSING))
                if i % 100000 == 0:
                    db.commit()
                    print(f'Title metadata: {i:,}/{entities:,}',flush=True)
            db.commit()
            staged = tmp/'sparrow.dat'
            shutil.copyfile(source, staged)
            with staged.open('r+b') as f:
                for i, offset, original in blocks(source, bp, buckets, 4096, ep+entities*16):
                    data = bytearray(original); pos = 0
                    while pos < len(data):
                        if pos+6 > len(data): raise ValueError('short alias record')
                        old, n = struct.unpack_from('<IH',data,pos)
                        if not 0 < n <= 127 or pos+6+n > len(data) or (old != MISSING and old >= entities):
                            raise ValueError('invalid alias record')
                        key = data[pos+6:pos+6+n].decode('utf-8')
                        row = db.execute('SELECT id FROM title WHERE key=?',(key,)).fetchone()
                        new = row[0] if row else old
                        if new != old:
                            struct.pack_into('<I',data,pos,new); changed += 1
                            disambiguated += old == MISSING and new != MISSING
                        ambiguous += new == MISSING
                        pos += 6+n
                    if data != original:
                        f.seek(offset); f.write(data)
                        f.seek(bp+i*16+12); f.write(struct.pack('<I',zlib.crc32(data)))
                f.flush(); os.fsync(f.fileno())
            report.update(sha256=sha(staged), source_index=str(source), source_index_sha256=source_sha,
                alias_policy='unique normalized stored enwiki title first; otherwise original alias',
                aliases_changed_by_title=changed, aliases_disambiguated_by_title=disambiguated,
                ambiguous_aliases=ambiguous, statement_audit=str(source)+'.statements.jsonl.gz',
                title_upgrade_note='Claims and entity blocks unchanged, including original unresolved-target handling')
            manifest = tmp/'sparrow.dat.json'
            manifest.write_text(json.dumps(report,indent=2)+'\n')
            staged.replace(output); manifest.replace(Path(str(output)+'.json'))
    return report


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('source',type=Path); p.add_argument('output',type=Path)
    args = p.parse_args()
    report = upgrade(args.source,args.output)
    print(json.dumps({k:report[k] for k in ('sha256','aliases_changed_by_title','ambiguous_aliases')},indent=2))


if __name__ == '__main__': main()
