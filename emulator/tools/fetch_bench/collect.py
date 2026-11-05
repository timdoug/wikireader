#!/usr/bin/env python3
"""Preserve a mounted card's fetch report before validating its identities."""
import datetime
import hashlib
import json
from pathlib import Path

from report import validate

HERE = Path(__file__).resolve().parent


def digest(data):
    return hashlib.sha256(data).hexdigest()


def collect(boot, destination=HERE):
    data = (boot / 'fetchq.log').read_bytes()
    destination.mkdir(parents=True, exist_ok=True)
    date = datetime.date.today().isoformat()
    report = destination / f'device-{date}-{digest(data)[:12]}.txt'
    if report.exists() and report.read_bytes() != data:
        raise ValueError('Refusing to replace different hardware evidence')
    report.write_bytes(data)
    record = dict(date=date, valid=False, report=dict(file=report.name, sha256=digest(data)))
    try:
        installed = json.loads((boot / 'fetchq.json').read_text())
        manifest, inputs = installed['manifest'], installed['files']
        record.update(manifest=manifest, inputs=inputs)
        if (set(inputs) != {'fetchq.app', 'fetchq.ico', 'kernel.elf', 'init.app', 'init.ini'} or
                inputs['fetchq.app'] != manifest['app_sha256'] or
                inputs['fetchq.ico'] != manifest['icon_sha256']):
            raise ValueError('Incomplete or mismatched installed inputs')
        for name, expected in inputs.items():
            if digest((boot / name).read_bytes()) != expected:
                raise ValueError(f'Changed card input: {name}')
        header, rows = validate(data.decode(), manifest)
        record.update(valid=True, cases=len(rows), header=header)
    except (KeyError, ValueError, OSError) as error:
        record['error'] = str(error)
    encoded = json.dumps(record, indent=2) + '\n'
    report.with_suffix('.json').write_text(encoded)
    (destination / 'device.json').write_text(encoded)
    if not record['valid']:
        raise ValueError(f'Raw report preserved at {report}; {record["error"]}')
    return report


def main():
    cards = list(Path('/Volumes').glob('*/fetchq.json'))
    if len(cards) != 1:
        raise SystemExit(f'Expected one mounted external-fetch probe card, found {len(cards)}')
    print(f'Preserved and verified external-fetch report: {collect(cards[0].parent)}')


if __name__ == '__main__':
    main()
