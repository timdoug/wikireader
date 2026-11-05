#!/usr/bin/env python3
"""Build a separate reader calibration app, preserving ordinary reader outputs."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'build/wr128/workflow-calibration'
ENTRIES = [('simple', 'Cat'), ('simple', 'Tokyo'), ('wikivoyage', 'Paris')]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    reader = OUT / 'reader'
    reader.mkdir(parents=True, exist_ok=True)
    for source in (ROOT / 'zim').iterdir():
        if source.is_file() and (source.suffix in ('.c', '.h', '.ico') or source.name == 'Makefile'):
            shutil.copy2(source, reader / source.name)
    # Reuse objects whose dependencies are unchanged. The normal reader
    # build and its archive are never modified by this isolated make.
    for name in ('build', 'lib'):
        target = reader / name
        target.mkdir(exist_ok=True)
        for source in (ROOT / 'zim' / name).iterdir():
            if source.is_file() and source.suffix in ('.o', '.d', '.h', '.a') and not (target / source.name).exists():
                shutil.copy2(source, target / source.name)
    for name in ('sparrow', 'wiki'):
        target = OUT / name
        if not target.exists():
            target.symlink_to(ROOT / name, target_is_directory=True)
    subprocess.run(['make', '-s', '-C', str(reader), '-j4'], check=True)
    shutil.copy2(reader / 'zim.app', OUT / 'zcal.app')
    shutil.copy2(reader / 'zim.ico', OUT / 'zcal.ico')
    listing = 'passes 3\n' + ''.join(f'{archive} | {title}\n' for archive, title in ENTRIES)
    (OUT / 'zimbench.lst').write_text(listing)
    record = {'version': 1, 'entries': [{'archive': a, 'title': t} for a, t in ENTRIES],
              'passes': 3, 'scroll_moves': [104, 104, -104, -104],
              'files': {name: sha(OUT / name) for name in ('zcal.app', 'zcal.ico', 'zimbench.lst')},
              'driver_sha256': sha(ROOT / 'zim/zim_bench.c'),
              'boot_sha256': {'kernel.elf': sha(ROOT / 'samo-lib/grifo/grifo.elf'),
                              'init.app': sha(ROOT / 'samo-lib/grifo/applications/init/init.app')}}
    (OUT / 'manifest.json').write_text(json.dumps(record, indent=2) + '\n')
    print(f'{OUT}/zcal.app: three passes, {len(ENTRIES)} articles, four viewport steps each')


if __name__ == '__main__':
    main()
