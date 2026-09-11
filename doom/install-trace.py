#!/usr/bin/env python3
"""Install a reviewed trace bundle on its identified, mounted WikiReader card."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import subprocess

FILES = {'doom.app', 'doom.ico', 'doom/doom1.wad', 'doomlog.on', 'doomrun.txt',
         'doomemu.log', 'doomref.json', 'doominit.bak', 'init.ini'}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('bundle', type=Path)
    parser.add_argument('--eject', action='store_true')
    args = parser.parse_args()
    bundle = args.bundle.resolve()
    manifest = json.loads((bundle / 'install.json').read_text())
    target = Path('/Volumes/WRBOOT')
    info = plistlib.loads(subprocess.check_output(['diskutil', 'info', '-plist', str(target)]))
    if (info.get('VolumeUUID') != manifest['volume_uuid'] or info.get('Internal')
            or info.get('FilesystemType') != 'msdos' or info.get('MountPoint') != str(target)
            or info.get('ParentWholeDisk') != manifest['whole_disk']):
        raise RuntimeError('Mounted card identity differs from the reviewed installation')
    if set(manifest['files']) != FILES:
        raise RuntimeError('Unexpected install file set')
    if info['FreeSpace'] < 8 * 1024 * 1024:
        raise RuntimeError('Insufficient space for the application, WAD and logs')
    for name, expected in manifest['preserve'].items():
        if name not in ('kernel.elf', 'init.app', 'init.ini') or sha(target / name) != expected:
            raise RuntimeError(f'Existing {name} changed since preparation')
    # Finish every check before the first card mutation. Existing files other
    # than init.ini may only be reused if they already match exactly.
    for name, expected in manifest['files'].items():
        src, dest = bundle / name, target / name
        if sha(src) != expected or dest.is_symlink() or dest.parent.is_symlink():
            raise RuntimeError(f'Invalid or changed staged file: {name}')
        if name != 'init.ini' and dest.exists() and sha(dest) != expected:
            raise RuntimeError(f'Refusing to replace existing {name}')
    temporary = target / 'doominit.new'
    if temporary.exists():
        raise RuntimeError('An unfinished launcher update exists: doominit.new')
    for name in sorted(FILES - {'init.ini'}):
        dest = target / name
        if not dest.exists():
            dest.parent.mkdir(exist_ok=True)
            with dest.open('xb') as out:
                out.write((bundle / name).read_bytes())
                out.flush()
                os.fsync(out.fileno())
        if sha(dest) != manifest['files'][name]:
            raise RuntimeError(f'Read-back mismatch: {name}')
        print(f'Verified {name}: {dest.stat().st_size} bytes', flush=True)
    # Switch the launcher only after the complete payload has been verified.
    with temporary.open('xb') as out:
        out.write((bundle / 'init.ini').read_bytes())
        out.flush()
        os.fsync(out.fileno())
    os.replace(temporary, target / 'init.ini')
    subprocess.run(['sync'], check=True)
    for name, expected in manifest['files'].items():
        if sha(target / name) != expected:
            raise RuntimeError(f'Final read-back mismatch: {name}')
    for name in ('kernel.elf', 'init.app'):
        if sha(target / name) != manifest['preserve'][name]:
            raise RuntimeError(f'Unexpected change to {name}')
    (bundle / 'installed.json').write_text(json.dumps(dict(verified=True,
        device=info['DeviceIdentifier'], volume_uuid=info['VolumeUUID'],
        files=manifest['files']), indent=2) + '\n')
    print('Doom trace installed; original launcher saved as doominit.bak.', flush=True)
    if args.eject:
        subprocess.run(['diskutil', 'eject', info['ParentWholeDisk']], check=True)
        print('Card safely ejected.', flush=True)


if __name__ == '__main__':
    main()
