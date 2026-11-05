#!/usr/bin/env python3
"""Find the returned bus probe card, verify its inputs and preserve the report."""
import datetime
import hashlib
import json
from pathlib import Path

from run import validate

HERE = Path(__file__).resolve().parent


def digest(data):
    return hashlib.sha256(data).hexdigest()


def collect(boot, destination=HERE):
    install = json.loads((boot / 'buspipe.json').read_text())
    manifest = install['manifest']
    required = {'buspipe.app', 'buspipe.ico', 'kernel.elf', 'init.app', 'init.ini'}
    if set(install['files']) != required:
        raise ValueError('Incomplete installed input identities')
    if install['files']['buspipe.app'] != manifest['app_sha256']:
        raise ValueError('Installed application differs from the probe manifest')
    for name, expected in install['files'].items():
        if digest((boot / name).read_bytes()) != expected:
            raise ValueError(f'Card input changed: {name}')
    data = (boot / 'buspipe.log').read_bytes()
    rows = validate(data.decode(), manifest)
    date = datetime.date.today().isoformat()
    report = destination / f'device-{date}-{digest(data)[:12]}.txt'
    record = {'date': date, 'manifest': manifest, 'inputs': install['files'],
              'report': {'file': report.name, 'sha256': digest(data)}, 'cases': len(rows)}
    destination.mkdir(parents=True, exist_ok=True)
    if report.exists() and report.read_bytes() != data:
        raise ValueError(f'Refusing to overwrite hardware evidence: {report}')
    report.write_bytes(data)
    (report.with_suffix('.json')).write_text(json.dumps(record, indent=2) + '\n')
    (destination / 'device.json').write_text(json.dumps(record, indent=2) + '\n')
    return report


def main():
    cards = list(Path('/Volumes').glob('*/buspipe.json'))
    if len(cards) != 1:
        raise SystemExit(f'Expected one mounted bus probe card, found {len(cards)}')
    report = collect(cards[0].parent)
    print(f'Collected and verified bus probe: {report}')


if __name__ == '__main__':
    main()
