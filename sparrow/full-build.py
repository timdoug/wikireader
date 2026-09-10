#!/usr/bin/env python3
"""Build from the verified local dump, then evaluate frozen binaries and questions.

No network requests. --start detaches the worker and leaves progress/log files.
The result is kept separately for review; this does not replace the demo card.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def status(directory, **fields):
    path = directory/'job.json'
    data = json.loads(path.read_text()) if path.exists() else {}
    data.update(fields, updated=time.time())
    tmp = directory/'job.tmp'
    tmp.write_text(json.dumps(data, indent=2)+'\n')
    tmp.replace(path)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('directory', type=Path)
    p.add_argument('--dump', type=Path, default=ROOT/'build/wikidata/wikidata-20260907-all.json.bz2')
    p.add_argument('--baseline', type=Path, default=ROOT/'build/sparrow/coverage-v2/before-cli')
    p.add_argument('--dataset', type=Path, default=ROOT/'build/sparrow/qald-9-plus/data')
    p.add_argument('--workers', type=int, default=6)
    p.add_argument('--start', action='store_true')
    p.add_argument('--worker', action='store_true', help=argparse.SUPPRESS)
    args = p.parse_args()
    directory = args.directory.resolve()
    if not 1 <= args.workers <= 64: p.error('workers must be between 1 and 64')
    if not args.worker:
        if directory.exists(): p.error('use a new output directory; existing build work is preserved')
        source = args.dump.resolve()
        record = json.loads(Path(str(source)+'.status.json').read_text())
        if record.get('state') != 'verified' or source.stat().st_size != record['expected_bytes'] or record.get('sha1') != record.get('sha1_expected'):
            p.error('source needs a completed, checksum-verified download record')
        if Path(record['path']).resolve() != source:
            p.error('download record names a different source')
        directory.mkdir(parents=True)
        bundle = directory/'frozen'; bundle.mkdir()
        for name in ('build.py', 'benchmark.py', 'demo-aliases.json'):
            shutil.copy2(ROOT/'sparrow'/name, bundle/name)
        shutil.copy2(ROOT/'sparrow/build/sparrow', bundle/'sparrow')
        shutil.copy2(args.baseline, bundle/'before-cli')
        for split in ('train', 'test'):
            shutil.copy2(args.dataset/f'qald_9_plus_{split}_wikidata.json', bundle/f'{split}.json')
        status(directory, state='prepared', dump=str(source), workers=args.workers,
               source_sha1=record['sha1'], source_verified_at=record['updated'],
               frozen_sha256={f.name:sha(f) for f in bundle.iterdir()},
               benchmark_policy='Untouched questions and stored answers; no gold-query execution or data injection')
        if args.start:
            with (directory/'run.log').open('ab') as log:
                child = subprocess.Popen([sys.executable, str(Path(__file__).resolve()), str(directory), '--worker'],
                    stdin=subprocess.DEVNULL, stdout=log, stderr=log, start_new_session=True, close_fds=True)
            print(json.dumps({'pid':child.pid, 'status':str(directory/'job.json'), 'progress':str(directory/'build-status.json'), 'log':str(directory/'run.log')}))
            return
    config = json.loads((directory/'job.json').read_text())
    bundle = directory/'frozen'
    for name, digest in config['frozen_sha256'].items():
        if sha(bundle/name) != digest: raise RuntimeError('Frozen build input changed: '+name)
    status(directory, state='building', pid=os.getpid(), started=time.time())
    try:
        subprocess.run([sys.executable, str(bundle/'build.py'), config['dump'], str(directory/'sparrow.dat'),
            '--snapshot', 'wikidata-20260907', '--workers', str(config['workers']),
            '--scratch', str(directory), '--reserve-gib', '24', '--aliases', str(bundle/'demo-aliases.json'),
            '--status', str(directory/'build-status.json')], check=True)
        status(directory, state='evaluating')
        for split in ('train', 'test'):
            for version, binary in [('before', 'before-cli'), ('after', 'sparrow')]:
                subprocess.run([sys.executable, str(bundle/'benchmark.py'), str(bundle/f'{split}.json'),
                    str(directory/'sparrow.dat'), '--cli', str(bundle/binary),
                    '--output', str(directory/f'{split}-{version}.json')], check=True)
        status(directory, state='complete', results={f'{split}-{v}':json.loads((directory/f'{split}-{v}.json').read_text())['counts']
               for split in ('train', 'test') for v in ('before', 'after')})
    except BaseException as error:
        status(directory, state='failed', error=str(error))
        raise


if __name__ == '__main__':
    main()
