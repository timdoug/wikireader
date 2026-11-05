#!/usr/bin/env python3
"""Archive the returned local-port report, including failed native runs."""
import datetime
import hashlib
import json
from pathlib import Path

from run import validate

HERE = Path(__file__).resolve().parent


def digest(data):
    return hashlib.sha256(data).hexdigest()


def collect(boot, destination=HERE):
    data = (boot / 'portq.log').read_bytes()
    destination.mkdir(parents=True, exist_ok=True)
    date = datetime.date.today().isoformat()
    report = destination / f'device-{date}-{digest(data)[:12]}.txt'
    if report.exists() and report.read_bytes() != data:
        raise ValueError(f'Refusing to overwrite hardware evidence: {report}')
    report.write_bytes(data)
    record = {'date': date, 'valid': False,
              'report': {'file': report.name, 'sha256': digest(data)}}
    try:
        install = json.loads((boot / 'portq.json').read_text())
        manifest, inputs = install['manifest'], install['files']
        record.update(manifest=manifest, inputs=inputs)
        required = {'portq.app', 'portq.ico', 'kernel.elf', 'init.app', 'init.ini'}
        if set(inputs) != required or inputs['portq.app'] != manifest['app_sha256']:
            raise ValueError('Incomplete or mismatched installed input identities')
        for name, expected in inputs.items():
            if digest((boot / name).read_bytes()) != expected:
                raise ValueError(f'Card input changed: {name}')
        rows = validate(data.decode(), manifest)
        record.update(valid=True, cases=len(rows))
    except (ValueError, KeyError, OSError) as error:
        record['error'] = str(error)
    # A failed return must invalidate the latest pointer too: never compare
    # against a silently retained successful report from an older probe.
    encoded = json.dumps(record, indent=2) + '\n'
    report.with_suffix('.json').write_text(encoded)
    (destination / 'device.json').write_text(encoded)
    if not record['valid']:
        raise ValueError(f'Report preserved at {report}; {record["error"]}')
    return report


def main():
    cards = list(Path('/Volumes').glob('*/portq.json'))
    if len(cards) != 1:
        raise SystemExit(f'Expected one mounted local-port probe card, found {len(cards)}')
    print(f'Collected and verified local-port probe: {collect(cards[0].parent)}')


if __name__ == '__main__':
    main()
