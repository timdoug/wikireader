#!/usr/bin/env python3
"""Prepare a private clone of an existing card image for reader measurements.

macOS only. Mounts the clone under a private temporary directory. Never
formats a volume or opens a raw block device. Original image is preserved.
"""
import argparse
import json
from pathlib import Path
import plistlib
import shutil
import stat
import subprocess
import tempfile

from build import ROOT, OUT
from run import sha


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / 'build/wr-card.dmg')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    args.source, args.out = args.source.resolve(), args.out.resolve()
    if not stat.S_ISREG(args.source.stat().st_mode):
        raise ValueError('Only a regular card image file is accepted')
    args.out.mkdir(parents=True, exist_ok=False)
    image = args.out / 'card.img'
    subprocess.run(['cp', '-c', str(args.source), str(image)], check=True)
    if image.stat().st_ino == args.source.stat().st_ino or sha(image) != sha(args.source):
        raise ValueError('Card image did not clone independently and exactly')
    manifest = json.loads((OUT / 'manifest.json').read_text())
    mountroot = Path(tempfile.mkdtemp(prefix='wremu-workflow-card-'))
    attached = plistlib.loads(subprocess.check_output([
        'hdiutil', 'attach', '-plist', '-noautoopen', '-mountroot', str(mountroot), str(image)]))
    entities = attached['system-entities']
    disk = entities[0]['dev-entry']
    try:
        boots = [Path(e['mount-point']) for e in entities if 'mount-point' in e and
                 (Path(e['mount-point']) / 'kernel.elf').exists()]
        if len(boots) != 1 or mountroot.resolve() not in boots[0].resolve().parents:
            raise ValueError('Unexpected cloned image mount')
        boot = boots[0]
        for name, expected in manifest['boot_sha256'].items():
            if sha(boot / name) != expected:
                raise ValueError(f'Fixture boot input differs: {name}')
        for name, expected in manifest['files'].items():
            if sha(OUT / name) != expected:
                raise ValueError(f'Calibration build changed: {name}')
            shutil.copyfile(OUT / name, boot / name)
        (boot / 'init.ini').write_text('zcal.ico : zcal.app bench cal calibrate\n')
        (boot / 'zimlog.on').write_bytes(b'')
        for name in ('zimbench.txt', 'zimboot.log', 'zimpage.log', 'dma.txt'):
            (boot / name).unlink(missing_ok=True)
        inputs = {name: sha(boot / name) for name in
                  (*manifest['files'], *manifest['boot_sha256'], 'init.ini', 'zimlog.on')}
        volumes = [Path(e['mount-point']) for e in entities if 'mount-point' in e and
                   list(Path(e['mount-point']).glob('*.zim'))]
        if len(volumes) != 1:
            raise ValueError('Missing fixture archives')
        archives = {p.name: {'size': p.stat().st_size, 'sha256': sha(p)}
                    for p in volumes[0].glob('*.zim')}
        for name, expected in inputs.items():
            if sha(boot / name) != expected:
                raise ValueError(f'Fixture copy did not verify: {name}')
        subprocess.run(['sync'], check=True)
    finally:
        subprocess.run(['hdiutil', 'detach', disk], check=True)
    record = {'manifest': manifest, 'files': inputs, 'archives': archives,
              'fixture_source': str(args.source), 'fixture_sha256': sha(image)}
    (args.out / 'inputs.json').write_text(json.dumps(record, indent=2) + '\n')
    try:
        mountroot.rmdir()
    except OSError:
        pass
    print(f'Prepared cloned workflow card: {image}')


if __name__ == '__main__':
    main()
