#!/usr/bin/env python3
"""Replace the returned port probe after its physical evidence is archived."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import uuid

from report import validate

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'build/wr128/fetchq'
OLD_ENTRY = b'portq.ico : portq.app off'
NEW_ENTRY = b'fetchq.ico : fetchq.app off'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def plan(boot, stage, validation, identity, device_dir, disk):
    if (disk.get('VolumeUUID') != identity['volume_uuid'] or disk.get('Internal') is not False or
            disk.get('BusProtocol') != 'USB' or disk.get('Content') != 'DOS_FAT_32' or
            not disk.get('WritableVolume')):
        raise ValueError('Unexpected reader card')
    previous = json.loads((device_dir / 'device.json').read_text())
    if (not previous.get('valid') or previous['inputs'] != identity['files'] or
            previous['manifest'] != identity['manifest']):
        raise ValueError('Current port probe must be collected and verified first')
    if (sha(device_dir / previous['report']['file']) != previous['report']['sha256'] or
            sha(boot / 'portq.log') != previous['report']['sha256']):
        raise ValueError('Returned port report differs from archived evidence')
    for name, expected in identity['files'].items():
        if sha(boot / name) != expected:
            raise ValueError(f'Changed returned input: {name}')
    preserved = identity['cleanup']['preserved_apps']
    for name, expected in preserved.items():
        if sha(boot / name) != expected:
            raise ValueError(f'Changed ordinary application: {name}')
    manifest = json.loads((stage / 'manifest.json').read_text())
    forecasts = json.loads((stage / 'prospective.json').read_text())
    if not forecasts['complete'] or forecasts['manifest'] != manifest or len(forecasts['models']) < 2:
        raise ValueError('Freeze matching prospective predictions before installation')
    run = json.loads((validation / 'results.json').read_text())
    if (not run['complete'] or run['manifest'] != manifest or run['cases'] != 600 or
            sha(validation / 'wremu') != run['emulator_sha256'] or
            sha(validation / 'fetchq/fetchq.log') != run['report_sha256']):
        raise ValueError('Incomplete or changed native validation')
    validate((validation / 'fetchq/fetchq.log').read_text(), manifest)
    for name, field in [('fetchq.app', 'app_sha256'), ('fetchq.ico', 'icon_sha256'), ('fetch.c', 'source_sha256')]:
        if sha(stage / name) != manifest[field]:
            raise ValueError(f'Changed validated build: {name}')
    for name in ('kernel.elf', 'init.app'):
        if sha(boot / name) != run['inputs']['fetchq'][name]:
            raise ValueError(f'Validated boot input differs: {name}')
    if run['inputs']['fetchq']['bench.app'] != manifest['app_sha256']:
        raise ValueError('Validation executed a different app')
    if run['inputs']['fetchq']['bench.ico'] != manifest['icon_sha256']:
        raise ValueError('Validation used a different icon')
    lines = (boot / 'init.ini').read_bytes().splitlines(keepends=True)
    entries = [line.strip() for line in lines if b':' in line]
    if len(entries) != 6 or entries[-1] != OLD_ENTRY or entries.count(OLD_ENTRY) != 1:
        raise ValueError('Expected the cleaned six-entry menu with portq last')
    launcher = b''.join(NEW_ENTRY + b'\n' if line.strip() == OLD_ENTRY else line for line in lines)
    return manifest, launcher, preserved


def write(path, data):
    with path.open('wb') as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())


def install(boot, stage, validation, identity, device_dir, disk, backup_root):
    manifest, launcher, preserved = plan(boot, stage, validation, identity, device_dir, disk)
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    backup = backup_root / f'card-backup-{stamp}-{uuid.uuid4().hex[:8]}'
    backup.mkdir(parents=True, exist_ok=False)
    for name in ('init.ini', 'portq.app', 'portq.ico', 'portq.json', 'portq.log',
                 'fetchq.app', 'fetchq.ico', 'fetchq.json', 'fetchq.log'):
        if (boot / name).exists():
            shutil.copy2(boot / name, backup / name)
            if sha(boot / name) != sha(backup / name):
                raise ValueError(f'Backup failed: {name}')
    for name in ('fetchq.app', 'fetchq.ico'):
        write(boot / name, (stage / name).read_bytes())
        if sha(boot / name) != sha(stage / name):
            raise ValueError(f'Copy verification failed: {name}')
    files = {name: sha(boot / name) for name in ('fetchq.app', 'fetchq.ico', 'kernel.elf', 'init.app')}
    files['init.ini'] = hashlib.sha256(launcher).hexdigest()
    record = dict(version=1, manifest=manifest, files=files, backup=str(backup),
                  volume_uuid=disk['VolumeUUID'], whole_disk=disk['ParentWholeDisk'],
                  install_id=str(uuid.uuid4()), installed_at=stamp,
                  prospective_sha256=sha(stage / 'prospective.json'), preserved_apps=preserved,
                  menu_entries=6, probe_position='rightmost icon on second row')
    encoded = (json.dumps(record, indent=2) + '\n').encode()
    write(boot / 'fetchq.json', encoded)
    write(boot / 'init.ini', launcher)
    for name, expected in {**files, **preserved}.items():
        if sha(boot / name) != expected:
            raise ValueError(f'Post-install verification failed: {name}')
    # Replaced probe files already have verified backups and an archived
    # physical report. Keep a single active probe on the cleaned card.
    for name in ('portq.app', 'portq.ico', 'portq.json', 'portq.log', 'fetchq.log'):
        (boot / name).unlink(missing_ok=True)
    write(stage / 'installed.json', encoded)
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--validation', type=Path, default=OUT / 'coherent-frontend-v2')
    args = parser.parse_args()
    identity = json.loads((ROOT / 'build/wr128/portq/installed.json').read_text())
    candidates = []
    for metadata in Path('/Volumes').glob('*/portq.json'):
        info = plistlib.loads(subprocess.check_output(['diskutil', 'info', '-plist', str(metadata.parent)]))
        if info.get('VolumeUUID') == identity['volume_uuid']:
            candidates.append((metadata.parent, info))
    if len(candidates) != 1:
        raise ValueError(f'Expected one known mounted reader card, found {len(candidates)}')
    boot, disk = candidates[0]
    record = install(boot, OUT, args.validation, identity,
                     ROOT / 'emulator/tools/port_bench', disk, OUT)
    subprocess.run(['sync'], check=True)
    for name, expected in {**record['files'], **record['preserved_apps']}.items():
        if sha(boot / name) != expected:
            raise ValueError(f'Final card verification failed: {name}')
    subprocess.run(['diskutil', 'eject', record['whole_disk']], check=True)
    print(f'Copied and verified fetchq in menu position 6; card ejected; backup {record["backup"]}')


if __name__ == '__main__':
    main()
