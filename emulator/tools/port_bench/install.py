#!/usr/bin/env python3
"""Copy the validated local-port probe to the known reader card (macOS)."""
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

from run import ROOT, sha, validate

OUT = ROOT / 'build/wr128/portq'
WORKFLOW = ROOT / 'emulator/tools/workflow_bench'
OLD_ENTRY = b'zcal.ico : zcal.app bench cal calibrate'
NEW_ENTRY = b'portq.ico : portq.app off'


def disk_info(path):
    return plistlib.loads(subprocess.check_output(['diskutil', 'info', '-plist', str(path)]))


def plan(boot, stage, validation, identity, device_dir, disk):
    if (disk.get('VolumeUUID') != identity['volume_uuid'] or
            disk.get('Internal') is not False or disk.get('BusProtocol') != 'USB' or
            disk.get('Content') != 'DOS_FAT_32' or not disk.get('WritableVolume')):
        raise ValueError('Unexpected reader boot volume')
    device = json.loads((device_dir / 'device.json').read_text())
    if device['install_id'] != identity['install_id'] or device['files'] != identity['files']:
        raise ValueError('Reader installation differs from archived hardware evidence')
    for key, name in [('report', 'zimbench.txt'), ('startup_report', 'zimboot.log')]:
        report = device[key]
        if (sha(device_dir / report['file']) != report['sha256'] or
                sha(boot / name) != report['sha256']):
            raise ValueError(f'Reader report must be archived before installation: {name}')
    for name, expected in identity['files'].items():
        if sha(boot / name) != expected:
            raise ValueError(f'Returned reader input changed: {name}')
    manifest = json.loads((stage / 'manifest.json').read_text())
    run = json.loads((validation / 'results.json').read_text())
    if (run['manifest'] != manifest or run['cases'] != manifest['cases'] or
            sha(validation / 'wremu') != run['emulator_sha256'] or
            sha(validation / 'portq/portq.log') != run['report_sha256']):
        raise ValueError('Probe differs from its validated emulator run')
    validate((validation / 'portq/portq.log').read_text(), manifest)
    if (sha(stage / 'portq.app') != manifest['app_sha256'] or
            sha(stage / 'portq.c') != manifest['source_sha256']):
        raise ValueError('Probe build changed since validation')
    for name, model_name in [('kernel.elf', 'kernel.elf'), ('init.app', 'init.app'),
                             ('portq.app', 'bench.app'), ('portq.ico', 'bench.ico')]:
        path = stage / name if name.startswith('portq.') else boot / name
        if sha(path) != run['inputs']['portq'][model_name]:
            raise ValueError(f'Validated boot/probe input differs: {name}')
    lines = (boot / 'init.ini').read_bytes().splitlines(keepends=True)
    entries = [line.strip() for line in lines if b':' in line]
    if len(entries) != 9 or entries[-1] != OLD_ENTRY or entries.count(OLD_ENTRY) != 1:
        raise ValueError('Expected reader calibration as the ninth menu entry')
    launcher = b''.join(NEW_ENTRY + b'\n' if line.strip() == OLD_ENTRY else line for line in lines)
    return manifest, launcher


def write(path, data):
    with path.open('wb') as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())


def install(boot, stage, validation, identity, device_dir, disk, backup_root):
    manifest, launcher = plan(boot, stage, validation, identity, device_dir, disk)
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    backup = backup_root / f'card-backup-{stamp}-{uuid.uuid4().hex[:8]}'
    backup.mkdir(parents=True, exist_ok=False)
    for name in ('portq.app', 'portq.ico', 'portq.json', 'portq.log', 'init.ini',
                 'zcal.json', 'zimbench.txt', 'zimboot.log'):
        if (boot / name).exists():
            shutil.copy2(boot / name, backup / name)
            if sha(boot / name) != sha(backup / name):
                raise ValueError(f'Backup verification failed: {name}')
    # Select the new app only after both application copies verify. Existing
    # reader applications, resources, logs and other menu entries are kept.
    for name in ('portq.app', 'portq.ico'):
        write(boot / name, (stage / name).read_bytes())
        if sha(boot / name) != sha(stage / name):
            raise ValueError(f'Copy verification failed: {name}')
    files = {name: sha(boot / name) for name in ('portq.app', 'portq.ico', 'kernel.elf', 'init.app')}
    files['init.ini'] = hashlib.sha256(launcher).hexdigest()
    record = {'version': 1, 'manifest': manifest, 'files': files,
              'install_id': str(uuid.uuid4()), 'installed_at': stamp,
              'volume_uuid': disk['VolumeUUID'], 'whole_disk': disk['ParentWholeDisk'],
              'backup': str(backup), 'physical_layout_captured': False}
    encoded = (json.dumps(record, indent=2) + '\n').encode()
    write(boot / 'portq.json', encoded)
    write(boot / 'init.ini', launcher)
    for name, expected in files.items():
        if sha(boot / name) != expected:
            raise ValueError(f'Installed input verification failed: {name}')
    # A prior run is retained in the verified backup, rather than presented
    # to the collector as evidence for this newly installed build.
    (boot / 'portq.log').unlink(missing_ok=True)
    write(stage / 'installed.json', encoded)
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--validation', type=Path, default=ROOT / 'build/wr128/dma-write-review/engine-portq')
    args = parser.parse_args()
    identity = json.loads((ROOT / 'build/wr128/workflow-calibration/installed.json').read_text())
    candidates = []
    for metadata in Path('/Volumes').glob('*/zcal.json'):
        disk = disk_info(metadata.parent)
        if disk.get('VolumeUUID') == identity['volume_uuid']:
            candidates.append((metadata.parent, disk))
    if len(candidates) != 1:
        raise ValueError(f'Expected the known mounted reader card, found {len(candidates)}')
    boot, disk = candidates[0]
    record = install(boot, OUT, args.validation, identity, WORKFLOW, disk, OUT)
    subprocess.run(['sync'], check=True)
    print(f'Installed probe as menu entry 9 on {boot}; backup: {record["backup"]}')
    print('Verify copies after remount, then eject before the hardware run.')


if __name__ == '__main__':
    main()
