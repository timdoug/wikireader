#!/usr/bin/env python3
"""Install the validated reader probe on the returned bus-probe card (macOS).

Preserves overwritten files locally. File identities establish the workload;
physical card allocation is a separate qualification requirement.
"""
import datetime
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import uuid

from build import OUT
from run import sha


def info(path):
    return plistlib.loads(subprocess.check_output(['diskutil', 'info', '-plist', str(path)]))


def write(path, data):
    with path.open('wb') as target:
        target.write(data)
        target.flush()
        os.fsync(target.fileno())


def main():
    cards = list(Path('/Volumes').glob('*/buspipe.json'))
    if len(cards) != 1:
        raise ValueError(f'Expected one returned bus probe card, found {len(cards)}')
    boot = cards[0].parent
    disk = info(boot)
    if (disk.get('Internal') is not False or disk.get('BusProtocol') != 'USB' or
            disk.get('Content') != 'DOS_FAT_32' or not disk.get('WritableVolume')):
        raise ValueError('Unexpected external boot volume')
    # Require archived, validated hardware evidence before changing its inputs.
    busdir = Path(__file__).resolve().parents[1] / 'bus_bench'
    bus = json.loads((busdir / 'device.json').read_text())
    if sha(busdir / bus['report']['file']) != bus['report']['sha256']:
        raise ValueError('Archived bus report changed')
    if sha(boot / 'buspipe.log') != bus['report']['sha256']:
        raise ValueError('Returned bus report differs from archived evidence')
    for name, expected in bus['inputs'].items():
        if sha(boot / name) != expected:
            raise ValueError(f'Returned bus input changed: {name}')
    manifest = json.loads((OUT / 'manifest.json').read_text())
    validation = json.loads((OUT.parent / 'idma-deadline-review/workflow/results.json').read_text())
    if not validation['complete'] or validation['inputs']['manifest'] != manifest:
        raise ValueError('Reader build differs from the validated probe')
    for name, expected in manifest['files'].items():
        if sha(OUT / name) != expected:
            raise ValueError(f'Reader build changed: {name}')
    for name, expected in manifest['boot_sha256'].items():
        if sha(boot / name) != expected:
            raise ValueError(f'Hardware boot input differs: {name}')
    original = (boot / 'init.ini').read_bytes()
    lines = original.splitlines(keepends=True)
    entries = [line.strip() for line in lines if b':' in line]
    if len(entries) != 9 or entries[-1] != b'buspipe.ico : buspipe.app':
        raise ValueError('Expected the bus probe as the ninth launcher entry')
    launcher = b''.join(b'zcal.ico : zcal.app bench cal calibrate\n'
                        if line.strip() == entries[-1] else line for line in lines)
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    backup = OUT / f'card-backup-{stamp}'
    backup.mkdir()
    names = (*manifest['files'], 'init.ini', 'zimlog.on', 'zcal.json', 'buspipe.json',
             'buspipe.log', 'zimbench.txt', 'zimboot.log', 'zimpage.log', 'dma.txt')
    for name in names:
        if (boot / name).exists():
            shutil.copyfile(boot / name, backup / name)
    whole = disk['ParentWholeDisk']
    listing = plistlib.loads(subprocess.check_output(['diskutil', 'list', '-plist', '/dev/' + whole]))
    volumes = [Path(p['MountPoint']) for d in listing['AllDisksAndPartitions']
               for p in d.get('Partitions', []) if 'MountPoint' in p and
               list(Path(p['MountPoint']).glob('*.zim'))]
    if len(volumes) != 1:
        raise ValueError('Expected one archive volume on this same card')
    archives = {p.name: {'size': p.stat().st_size, 'sha256': sha(p)}
                for p in volumes[0].glob('*.zim')}
    if not archives:
        raise ValueError('Missing hardware archives')
    for name in manifest['files']:
        write(boot / name, (OUT / name).read_bytes())
    write(boot / 'init.ini', launcher)
    write(boot / 'zimlog.on', b'')
    for name in ('zimbench.txt', 'zimboot.log', 'zimpage.log', 'dma.txt', 'buspipe.json'):
        (boot / name).unlink(missing_ok=True)
    files = {name: sha(boot / name) for name in
             (*manifest['files'], *manifest['boot_sha256'], 'init.ini', 'zimlog.on')}
    record = {'version': 1, 'install_id': str(uuid.uuid4()), 'installed_at': stamp,
              'manifest': manifest, 'files': files, 'archives': archives,
              'volume_uuid': disk['VolumeUUID'], 'partition': disk['DeviceIdentifier'],
              'whole_disk': whole, 'backup': str(backup),
              'launcher_tap': [200, 172, 100000000]}
    data = (json.dumps(record, indent=2) + '\n').encode()
    write(boot / 'zcal.json', data)
    write(OUT / 'installed.json', data)
    for name, expected in files.items():
        if sha(boot / name) != expected:
            raise ValueError(f'Installed copy failed verification: {name}')
    subprocess.run(['sync'], check=True)
    print(f'Installed reader probe as entry 9 on {boot}; backup: {backup}')
    print('Verify the files after remount and eject before running on hardware.')


if __name__ == '__main__':
    main()
