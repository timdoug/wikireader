#!/usr/bin/env python3
"""Stream Wikidata JSON/JSONL into a disk-backed staging DB, then a SPRWQA1 file.
Only the host uses SQLite/JSON; the C33 reads the emitted flat binary.
"""
import argparse
import collections
import calendar
import contextlib
import datetime
import decimal
import gzip
import bz2
import hashlib
import itertools
import io
import json
import os
from pathlib import Path
import re
import sqlite3
import struct
import sys
import tempfile
import shutil
import zlib
import time

try:
    from orjson import loads as json_loads
except ImportError:
    json_loads = json.loads

HOST_WORKERS = 1
STATUS_PATH = None
RESERVE_GIB = 24


def progress(state, report):
    if STATUS_PATH:
        value = dict(report, state=state, pid=os.getpid(), updated=time.time())
        tmp = STATUS_PATH.with_suffix('.tmp')
        tmp.write_text(json.dumps(value, indent=2) + '\n')
        tmp.replace(STATUS_PATH)

PROPERTIES = {6, 17, 19, 20, 22, 25, 26, 27, 30, 35, 36, 37, 38, 39, 40,
              47, 50, 57, 61, 69, 84, 86, 98, 106, 112, 119, 122, 127, 136,
              159, 166, 170, 177, 178, 184, 277, 287, 421, 509, 551, 569, 570,
              571, 576, 577, 585, 737, 1082, 1128, 1303, 1477, 2044, 2046,
              2047, 2048, 2142, 2295}
MISSING = 0xffffffff
OFFICE = 65534
UNSAFE, START, END, ASOF = 1, 2, 4, 8
PARTIAL_TIME = 16
UNRESOLVED_OBJECT = 32
MAX_TEXT, MAX_KEY, MAX_BLOCK, MAX_BUCKET = 255, 127, 65536, 4096


def normalize(s):
    s = s.translate(str.maketrans('ABCDEFGHIJKLMNOPQRSTUVWXYZ_', 'abcdefghijklmnopqrstuvwxyz '))
    return re.sub('[ \t\r\n]+', ' ', s).strip(' ')


def hash_key(s):
    h = 2166136261
    for c in s.encode('utf-8'):
        h = ((h ^ c) * 16777619) & MISSING
    h = ((h ^ (h >> 16)) * 0x7feb352d) & MISSING
    h = ((h ^ (h >> 15)) * 0x846ca68b) & MISSING
    return h ^ (h >> 16)


def qid(s):
    m = re.fullmatch(r'(?:https?://www.wikidata.org/entity/)?Q([1-9][0-9]*)', str(s))
    if not m or int(m[1]) >= MISSING:
        raise ValueError(f'invalid entity ID: {s!r}')
    return int(m[1])


def staged_claim(raw):
    # Compression is confined to host staging. The device format is unchanged.
    return json_loads(zlib.decompress(raw) if isinstance(raw, bytes) else raw)


def entities(path):
    with open(path, 'rb') as probe:
        magic = probe.read(3)
    if magic == b'BZh' and HOST_WORKERS > 1:
        import indexed_bzip2
        source = io.BufferedReader(indexed_bzip2.IndexedBzip2File(str(path), parallelization=HOST_WORKERS), buffer_size=1024*1024)
    else:
        opener = gzip.open if magic[:2] == b'\x1f\x8b' else bz2.open if magic == b'BZh' else open
        source = opener(path, 'rb')
    with source as f:
        for number, line in enumerate(f, 1):
            line = line.strip()
            if line in (b'', b'[', b']'):
                continue
            try:
                value = json_loads(line.removesuffix(b','))
                if isinstance(value, dict) and 'entities' not in value:
                    yield value
                else:
                    raise ValueError('expected one entity object per line')
            except (ValueError, TypeError) as e:
                raise ValueError(f'{path}:{number}: {e}') from e


def time_value(v):
    """Exact Gregorian days only enter joins/arithmetic; preserve other display precision."""
    raw = v.get('time', '')
    precision = v.get('precision', 0)
    m = re.fullmatch(r'\+([0-9]{4})-([0-9]{2})-([0-9]{2})T00:00:00Z', raw)
    exact = 0
    if m and precision == 11 and v.get('calendarmodel', '').endswith('/Q1985727') and not any(v.get(k, 0) for k in ('before', 'after', 'timezone')):
        try:
            y, mo, d = map(int, m.groups())
            datetime.date(y, mo, d)
            exact = y * 10000 + mo * 100 + d
        except ValueError:
            pass
    display = raw.lstrip('+').split('T')[0]
    if precision == 9:
        display = display.split('-')[0]
    elif precision == 10:
        display = '-'.join(display.split('-')[:2])
    if not exact:
        display += f' (precision {precision}; calendar {v.get("calendarmodel", "unknown").rsplit("/", 1)[-1]})'
    return exact, display


def disk_guard(directory, reserve_gib):
    if shutil.disk_usage(directory).free < reserve_gib * 1024**3:
        raise RuntimeError(f'Free space fell below {reserve_gib} GiB; stopping staging')


def qualifier_date(v):
    """YYYY0000 and YYYYMM00 preserve year/month precision; never invent a day."""
    exact, _ = time_value(v)
    if exact:
        return exact
    m = re.fullmatch(r'\+([0-9]{4})-([0-9]{2})-([0-9]{2})T00:00:00Z', v.get('time', ''))
    if not m or not v.get('calendarmodel', '').endswith('/Q1985727') or any(v.get(k, 0) for k in ('before', 'after', 'timezone')):
        return 0
    y, mo, d = map(int, m.groups())
    if not (1 <= y <= 9999 and 0 <= mo <= 12 and 0 <= d <= 31):
        return 0
    if v.get('precision') == 9:
        return y * 10000
    if v.get('precision') == 10 and mo:
        return y * 10000 + mo * 100
    return 0


def date_bound(d, latest=False):
    y, mo, day = d // 10000, d // 100 % 100, d % 100
    mo = mo or (12 if latest else 1)
    day = day or (calendar.monthrange(y, mo)[1] if latest else 1)
    return y * 10000 + mo * 100 + day


def import_dump(db, path, report, scratch=None, reserve_gib=24, limit=0):
    db.executescript('''
        CREATE TABLE entity(q INTEGER PRIMARY KEY, label TEXT, title TEXT, aliases TEXT);
        CREATE TABLE claim(subject INTEGER, prop INTEGER, raw TEXT);
        CREATE TABLE needed(q INTEGER PRIMARY KEY);
        CREATE TABLE office(position INTEGER, holder INTEGER, raw TEXT);
    ''')
    for item in itertools.islice(entities(path), limit or None):
        if item.get('type', 'item') != 'item' or not re.fullmatch(r'Q[1-9][0-9]*', item.get('id', '')):
            continue
        q = qid(item['id'])
        # XML revision JSON can serialize empty PHP maps as [] instead of {}.
        title = (item.get('sitelinks') or {}).get('enwiki', {}).get('title', '')
        labels = item.get('labels') or {}
        label = labels.get('en', {}).get('value', '') or labels.get('mul', {}).get('value', '') or title or item['id']
        aliases = [a['value'] for lang in ('en', 'mul') for a in (item.get('aliases') or {}).get(lang, [])]
        db.execute('INSERT INTO entity VALUES(?,?,?,?)', (q, label, title, json.dumps(aliases)))
        report['entities_scanned'] += 1
        if title:
            db.execute('INSERT OR IGNORE INTO needed VALUES(?)', (q,))
        # Stage selected properties for all subjects: non-Wikipedia bridge
        # entities and units must remain available after the dump has passed.
        for prop, claims in (item.get('claims') or {}).items():
            if not re.fullmatch(r'P[1-9][0-9]*', prop) or int(prop[1:]) not in PROPERTIES:
                continue
            p = int(prop[1:])
            for claim in claims:
                if claim.get('rank') == 'deprecated':
                    report['deprecated_skipped'] += 1
                    continue
                raw = zlib.compress(json.dumps(claim, ensure_ascii=False, separators=(',', ':')).encode(), 1)
                db.execute('INSERT INTO claim VALUES(?,?,?)', (q, p, raw))
                if p == 39:
                    snak = claim.get('mainsnak', {})
                    v = snak.get('datavalue', {}).get('value', {})
                    if snak.get('snaktype') == 'value' and isinstance(v, dict) and 'id' in v:
                        position = qid(v['id'])
                        db.execute('INSERT INTO office VALUES(?,?,?)', (position, q, raw))
                        db.execute('INSERT OR IGNORE INTO needed VALUES(?)', (position,))
                        db.execute('INSERT OR IGNORE INTO needed VALUES(?)', (q,))
                for snak in [claim.get('mainsnak', {})] + list(itertools.chain.from_iterable((claim.get('qualifiers') or {}).values())):
                    v = snak.get('datavalue', {}).get('value')
                    if isinstance(v, dict):
                        for target in [v.get('id'), v.get('unit')]:
                            if target and target != '1':
                                try:
                                    db.execute('INSERT OR IGNORE INTO needed VALUES(?)', (qid(target),))
                                except ValueError:
                                    pass
        if report['entities_scanned'] % 10000 == 0:
            db.commit()
            if scratch: disk_guard(scratch, reserve_gib)
            print(f"scanned {report['entities_scanned']:,} entities", file=sys.stderr)
            if scratch:
                report['scratch_bytes'] = sum(p.stat().st_size for p in Path(scratch).iterdir() if p.is_file())
                report['free_bytes'] = shutil.disk_usage(scratch).free
            progress('importing', report)
    db.commit()
    progress('indexing staged claims', report)
    finish_staging(db)


def finish_staging(db):
    db.executescript('''
        CREATE INDEX claim_subject ON claim(subject,prop);
        CREATE INDEX office_position ON office(position);
        CREATE TABLE dense(id INTEGER PRIMARY KEY, q INTEGER UNIQUE);
        INSERT INTO dense(q) SELECT q FROM entity JOIN needed USING(q) ORDER BY q;
        UPDATE dense SET id=id-1;
    ''')
    db.commit()


def metadata(db, q):
    row = db.execute('SELECT id,label,title FROM dense JOIN entity USING(q) WHERE q=?', (q,)).fetchone()
    return row if row else (MISSING, f'Q{q}', '')


def claim_record(db, raw, prop, holder=None):
    c = staged_claim(raw)
    s = c.get('mainsnak', {})
    v = s.get('datavalue', {}).get('value')
    typ = s.get('datavalue', {}).get('type')
    kind, obj, date, start, end, asof, flags = 4, MISSING, 0, 0, 0, 0, 0
    object_qid = 0
    text, unit = 'Unknown value', ''
    if holder is not None:
        object_qid = holder
        obj, text, _ = metadata(db, holder)
        kind = 1
        if obj == MISSING:
            flags |= UNRESOLVED_OBJECT
    elif s.get('snaktype') != 'value':
        text = s.get('snaktype', 'missing value')
        flags |= UNSAFE
    elif typ == 'wikibase-entityid' and isinstance(v, dict) and 'id' in v:
        object_qid = qid(v['id'])
        obj, text, _ = metadata(db, qid(v['id']))
        kind = 1
        if obj == MISSING:
            flags |= UNRESOLVED_OBJECT
    elif typ == 'time' and isinstance(v, dict):
        kind = 2
        date, text = time_value(v)
    elif typ == 'quantity' and isinstance(v, dict):
        kind = 3
        text = v['amount'].removeprefix('+')
        decimal.Decimal(text)  # validate, never round through a float
        if v.get('unit', '1') != '1':
            _, unit, _ = metadata(db, qid(v['unit']))
        if v.get('lowerBound') is not None or v.get('upperBound') is not None:
            text += f' [{v.get("lowerBound", "?")}, {v.get("upperBound", "?")}]'
    elif isinstance(v, str):
        text = v
    elif typ == 'monolingualtext' and isinstance(v, dict):
        text = v.get('text', '')
        if v.get('language') not in ('en', 'mul') or not text:
            flags |= UNSAFE
    else:
        flags |= UNSAFE
    qualifiers = c.get('qualifiers') or {}
    for p, snaks in qualifiers.items():
        # These describe evidence, ordering, elections, or adjacent holders;
        # they do not restrict the subject/value pair. Originals are retained
        # in the statement audit. Scope/part qualifiers are NOT in this list.
        if p in ('P1365', 'P1366', 'P1545', 'P805', 'P2715', 'P2572', 'P7452'):
            continue
        if p not in ('P580', 'P582', 'P585'):
            flags |= UNSAFE
            continue
        if len(snaks) != 1 or snaks[0].get('snaktype') != 'value' or snaks[0].get('datavalue', {}).get('type') != 'time':
            flags |= UNSAFE
            continue
        d = qualifier_date(snaks[0]['datavalue']['value'])
        if not d:
            flags |= UNSAFE
        elif d % 100 == 0:
            flags |= PARTIAL_TIME
        if p == 'P580':
            start = d
            flags |= START
        elif p == 'P582':
            end = d
            flags |= END
        else:
            asof = d
            flags |= ASOF
    if start and end and date_bound(end, latest=True) < date_bound(start):
        flags |= UNSAFE
    source = c.get('id', '')
    strings = [x.encode('utf-8') for x in (text, unit, source)]
    if any(len(x) > MAX_TEXT or b'\0' in x for x in strings):
        flags |= UNSAFE
        strings = [b'Value exceeds display limit', b'', b'']
    header = struct.pack('<HBBIiiiiHHHHI', prop, kind, int(c.get('rank') == 'preferred'),
                         obj, date, start, end, asof, flags, *(len(x) for x in strings), object_qid)
    return header + b''.join(strings), bool(flags & UNSAFE)


def pad(f):
    n = (-f.tell()) & 511
    f.write(b'\0' * n)


def make_aliases(db, overrides, report):
    db.execute('CREATE TABLE alias(key TEXT, id INTEGER, PRIMARY KEY(key,id)) WITHOUT ROWID')
    db.execute('CREATE TABLE title_key(key TEXT, id INTEGER, PRIMARY KEY(key,id)) WITHOUT ROWID')
    for id_, label, title, aliases in db.execute('SELECT id,label,title,aliases FROM dense JOIN entity USING(q) ORDER BY id'):
        key = normalize(title)
        if key and len(key.encode()) <= MAX_KEY and '\0' not in key:
            db.execute('INSERT OR IGNORE INTO title_key VALUES(?,?)', (key, id_))
        for name in {label, title, *json.loads(aliases)}:
            key = normalize(name)
            if key and len(key.encode()) <= MAX_KEY and '\0' not in key:
                db.execute('INSERT OR IGNORE INTO alias VALUES(?,?)', (key, id_))
            elif key:
                report['aliases_unsupported'] += 1
    if overrides:
        for name, target in json.loads(Path(overrides).read_text()).items():
            key = normalize(name)
            id_, _, _ = metadata(db, qid(target))
            if id_ == MISSING or not key or len(key.encode()) > MAX_KEY or '\0' in key:
                raise ValueError(f'invalid alias or missing target: {name}: {target}')
            db.execute('INSERT OR IGNORE INTO alias VALUES(?,?)', (key, id_))
    db.create_function('sparrow_hash', 1, hash_key, deterministic=True)
    db.executescript(f'''
        CREATE TABLE canonical_title(key TEXT PRIMARY KEY, id INTEGER);
        INSERT INTO canonical_title SELECT key,CASE WHEN COUNT(*)=1 THEN MIN(id) ELSE {MISSING} END FROM title_key GROUP BY key;
        CREATE TABLE key(id INTEGER, key TEXT PRIMARY KEY, hash INTEGER, bucket INTEGER, bytes INTEGER);
        INSERT INTO key(id,key,hash,bytes)
          SELECT COALESCE((SELECT id FROM canonical_title WHERE canonical_title.key=alias.key),
                         CASE WHEN COUNT(*)=1 THEN MIN(id) ELSE {MISSING} END),key,sparrow_hash(key),6+length(CAST(key AS BLOB))
          FROM alias GROUP BY alias.key;
    ''')
    report['alias_policy'] = 'unique normalized enwiki title first; otherwise unambiguous alias'
    report['aliases_disambiguated_by_title'] = db.execute('SELECT COUNT(*) FROM (SELECT key FROM alias GROUP BY key HAVING COUNT(*)>1) JOIN canonical_title USING(key) WHERE id!=?', (MISSING,)).fetchone()[0]
    report['ambiguous_aliases'] = db.execute('SELECT COUNT(*) FROM key WHERE id=?', (MISSING,)).fetchone()[0]
    report['aliases'] = db.execute('SELECT COUNT(*) FROM key').fetchone()[0]
    total = db.execute('SELECT COALESCE(SUM(bytes),0) FROM key').fetchone()[0]
    buckets = 1
    while buckets * 2048 < total:
        buckets *= 2
    while True:
        db.execute('UPDATE key SET bucket=hash & ?', (buckets - 1,))
        maximum = db.execute('SELECT COALESCE(MAX(n),0) FROM (SELECT SUM(bytes) AS n FROM key GROUP BY bucket)').fetchone()[0]
        if maximum <= MAX_BUCKET:
            break
        buckets *= 2
        if buckets > 1 << 28:
            raise ValueError('alias bucket cannot fit (hash collision overload)')
    db.execute('CREATE INDEX key_bucket ON key(bucket,key)')
    db.commit()
    return buckets


def emit(db, output, snapshot, overrides, report):
    disk_guard(output.parent, RESERVE_GIB)
    progress('building aliases', report)
    buckets = make_aliases(db, overrides, report)
    count = db.execute('SELECT COUNT(*) FROM dense').fetchone()[0]
    if count >= MISSING:
        raise ValueError('entity count exceeds v1 format')
    entities_at = (512 + buckets * 16 + 511) & ~511
    report.update(entities=count, buckets=buckets, snapshot=snapshot,
                  properties=sorted(PROPERTIES), format='SPRWQA1')
    provenance = output.with_suffix(output.suffix + '.statements.jsonl.gz')
    with output.open('w+b') as f, gzip.open(provenance, 'wt', encoding='utf-8') as audit:
        f.seek(entities_at + count * 16)
        pad(f)
        key_cursor = iter(db.execute('SELECT bucket,id,key FROM key ORDER BY bucket,key'))
        row = next(key_cursor, None)
        for bucket in range(buckets):
            if bucket % 10000 == 0: disk_guard(output.parent, RESERVE_GIB)
            start = f.tell()
            checksum = 0
            while row and row[0] == bucket:
                data = row[2].encode()
                record = struct.pack('<IH', row[1], len(data)) + data
                checksum = zlib.crc32(record, checksum)
                f.write(record)
                row = next(key_cursor, None)
            length = f.tell() - start
            pad(f)
            end = f.tell()
            f.seek(512 + bucket * 16)
            f.write(struct.pack('<QII', start, length, checksum))
            f.seek(end)
        for id_, q, label, title in db.execute('SELECT id,q,label,title FROM dense JOIN entity USING(q) ORDER BY id'):
            if id_ % 10000 == 0:
                disk_guard(output.parent, RESERVE_GIB)
                report['entities_emitted'] = id_
                progress('writing device index', report)
            strings = [x.encode() for x in (label, title)]
            if len(strings[0]) > MAX_TEXT or b'\0' in strings[0]:
                strings[0] = f'Q{q}'.encode()
                report['labels_shown_as_qid'] += 1
            if len(strings[1]) > MAX_TEXT or b'\0' in strings[1]:
                strings[1] = b''
                report['oversize_article_links_omitted'] += 1
            groups = collections.defaultdict(list)
            claims = db.execute('SELECT prop,raw FROM claim WHERE subject=? AND prop!=39 ORDER BY prop,raw', (q,))
            terms = ((OFFICE, raw, holder) for holder, raw in db.execute('SELECT holder,raw FROM office WHERE position=? ORDER BY holder,raw', (q,)))
            for prop, raw, holder in itertools.chain(((p, r, None) for p, r in claims), terms):
                record, unsafe = claim_record(db, raw, prop, holder)
                groups[prop].append(record)
                report['claims'] += 1
                report['unsafe_claims'] += unsafe
                report['unresolved_object_claims'] += bool(struct.unpack_from('<H',record,24)[0] & UNRESOLVED_OBJECT)
                audit.write(json.dumps({'subject': f'Q{q}', 'property': prop, 'holder': holder,
                                        'statement': staged_claim(raw)}, ensure_ascii=False) + '\n')
            records = [record for p in sorted(groups) for record in groups[p]]
            # One huge office or bibliographic record must not prevent an
            # otherwise usable index. Preserve all originals in the audit,
            # replacing whole oversized properties with an unsafe sentinel.
            # Never keep a plausible-looking truncated answer set.
            while 12 + sum(map(len, strings)) + sum(map(len, records)) > MAX_BLOCK or len(records) > 65535:
                prop = max(groups, key=lambda p:sum(map(len, groups[p])))
                text = b'Property exceeds device storage limit'
                report['device_claims_omitted_for_size'] += len(groups[prop])
                report['oversize_property_groups'] += 1
                groups[prop] = [struct.pack('<HBBIiiiiHHHHI', prop, 4, 1, MISSING,
                                           0, 0, 0, 0, UNSAFE, len(text), 0, 0, 0) + text]
                records = [record for p in sorted(groups) for record in groups[p]]
            report['stored_claim_records'] += len(records)
            data = struct.pack('<IHHHH', q, *(len(x) for x in strings), len(records), 0) + b''.join(strings) + b''.join(records)
            start = f.tell()
            f.write(data)
            pad(f)
            end = f.tell()
            f.seek(entities_at + id_ * 16)
            f.write(struct.pack('<QII', start, len(data), zlib.crc32(data)))
            f.seek(end)
        size = f.tell()
        f.truncate(size)
        h = struct.pack('<8sIIQIIQQ32s', b'SPRWQA1\0', 1, 128, size, buckets, count,
                        512, entities_at, snapshot.encode())
        f.seek(0)
        f.write(h + struct.pack('<I', zlib.crc32(h)) + b'\0' * (124 - len(h)))
        f.flush()
        os.fsync(f.fileno())
    report['bytes'] = size
    digest = hashlib.sha256()
    with output.open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            digest.update(block)
    report['sha256'] = digest.hexdigest()


def main():
    global HOST_WORKERS, STATUS_PATH, RESERVE_GIB
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('dump', type=Path)
    p.add_argument('output', type=Path)
    p.add_argument('--snapshot', required=True, help='source dump date/identity, at most 31 UTF-8 bytes')
    p.add_argument('--aliases', type=Path, help='JSON mapping additional aliases to QIDs; collisions abstain')
    p.add_argument('--reserve-gib', type=int, default=24)
    p.add_argument('--limit', type=int, default=0, help='development only: first N input records; marks output as partial')
    p.add_argument('--scratch', type=Path, help='directory for disk-backed staging; needs ample space')
    p.add_argument('--workers', type=int, default=1, help='parallel bzip2 decode workers; needs indexed_bzip2 when greater than one')
    p.add_argument('--status', type=Path, help='write build progress JSON here')
    args = p.parse_args()
    if not 1 <= args.workers <= 64: p.error('workers must be between 1 and 64')
    HOST_WORKERS, STATUS_PATH = args.workers, args.status
    RESERVE_GIB = args.reserve_gib
    if STATUS_PATH: STATUS_PATH.parent.mkdir(parents=True, exist_ok=True)
    if not args.snapshot or len(args.snapshot.encode()) > 31 or '\0' in args.snapshot:
        p.error('snapshot must be 1..31 UTF-8 bytes')
    if args.output.exists() or args.output.with_suffix(args.output.suffix + '.statements.jsonl.gz').exists():
        p.error('output already exists')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    report = collections.defaultdict(int)
    report['input_record_limit'] = args.limit
    report['source_path'] = str(args.dump.resolve())
    report['started'] = time.time()
    report['decode_workers'] = HOST_WORKERS
    report['staging_claim_codec'] = 'zlib-json-v1'
    if args.limit < 0: p.error('limit must be nonnegative')
    if args.limit and not args.snapshot.startswith('partial-'): p.error('limited builds require a partial- snapshot name')
    with tempfile.TemporaryDirectory(prefix='sparrow-', dir=args.scratch) as tmp:
        with contextlib.closing(sqlite3.connect(Path(tmp) / 'stage.db')) as db:
            db.execute('PRAGMA journal_mode=OFF')
            db.execute('PRAGMA synchronous=OFF')
            db.execute('PRAGMA temp_store=FILE')
            db.execute('PRAGMA cache_size=-65536')
            disk_guard(tmp, args.reserve_gib)
            import_dump(db, args.dump, report, tmp, args.reserve_gib, args.limit)
            staging = Path(tmp) / 'sparrow.dat'
            disk_guard(tmp, args.reserve_gib)
            emit(db, staging, args.snapshot, args.aliases, report)
            # Copy beside the final path then rename: works across filesystems
            # and a interrupted build never exposes a partly written index.
            for src, dst in [(staging, args.output),
                             (staging.with_suffix('.dat.statements.jsonl.gz'), args.output.with_suffix(args.output.suffix + '.statements.jsonl.gz'))]:
                if shutil.disk_usage(dst.parent).free < src.stat().st_size + args.reserve_gib * 1024**3:
                    raise RuntimeError('Insufficient space for atomic publication plus reserve')
                with tempfile.NamedTemporaryFile(dir=dst.parent, delete=False) as f:
                    temporary = Path(f.name)
                    with src.open('rb') as inp:
                        shutil.copyfileobj(inp, f)
                    f.flush()
                    os.fsync(f.fileno())
                os.replace(temporary, dst)
    args.output.with_suffix(args.output.suffix + '.json').write_text(json.dumps(dict(report), indent=2) + '\n')
    progress('complete', report)
    print(json.dumps(dict(report), indent=2))


if __name__ == '__main__':
    main()
