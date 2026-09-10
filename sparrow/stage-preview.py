#!/usr/bin/env python3
"""Export a partial device index from a consistent snapshot of an active import.

macOS/APFS only: hold SQLite's shared read lock while cloning the database
file with copy-on-write. Never fall back to a slow copy under that lock.
All indexing and export operate on the clone after releasing the source lock.
"""
import argparse
import collections
import json
from pathlib import Path
import sqlite3
import subprocess
import time
import build as builder


def snapshot(source, destination):
    if destination.exists(): raise RuntimeError('snapshot already exists')
    with sqlite3.connect(source.resolve().as_uri()+'?mode=ro', uri=True, timeout=1) as db:
        if db.execute('PRAGMA journal_mode').fetchone()[0] == 'wal':
            raise RuntimeError('preview requires a non-WAL staging database')
        db.execute('BEGIN')
        # Force a shared file lock, excluding any writer's disk-page flush.
        db.execute('SELECT name FROM sqlite_master LIMIT 1').fetchone()
        try:
            subprocess.run(['cp', '-c', str(source), str(destination)], check=True, timeout=2)
        finally:
            db.rollback()


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('job', type=Path)
    p.add_argument('output', type=Path)
    args=p.parse_args()
    config=json.loads((args.job/'job.json').read_text())
    state=json.loads((args.job/'build-status.json').read_text())
    if config['state']!='building' or state['state']!='importing':
        p.error('the source job must still be importing')
    sources=list(args.job.glob('sparrow-*/stage.db'))
    if len(sources)!=1: p.error('expected exactly one active staging database')
    if args.output.exists() and any(args.output.iterdir()): p.error('output already exists')
    args.output.mkdir(parents=True,exist_ok=True)
    copy=args.output/'snapshot.db'
    builder.disk_guard(args.output,24)
    snapshot(sources[0],copy)
    print('Consistent database clone created; full import continues.',flush=True)
    builder.STATUS_PATH=args.output/'status.json'
    with sqlite3.connect(copy) as db:
        if db.execute('PRAGMA quick_check').fetchone()!=('ok',):
            raise RuntimeError('snapshot integrity check failed')
        db.execute('PRAGMA journal_mode=OFF')
        db.execute('PRAGMA synchronous=OFF')
        db.execute('PRAGMA temp_store=FILE')
        db.execute('PRAGMA cache_size=-65536')
        count=db.execute('SELECT COUNT(*) FROM entity').fetchone()[0]
        report=collections.defaultdict(int, partial_snapshot=True, entities_scanned=count,
            source_path=config['dump'], source_sha1=config['source_sha1'], source_job=str(args.job),
            started=time.time(), staging_claim_codec='zlib-json-v1')
        builder.progress('indexing snapshot',report)
        builder.finish_staging(db)
        builder.emit(db,args.output/'sparrow.dat',f'partial-20260907-{count}',None,report)
        (args.output/'sparrow.dat.json').write_text(json.dumps(dict(report),indent=2)+'\n')
        builder.progress('complete',report)
        print(json.dumps(dict(report),indent=2))


if __name__=='__main__':main()
