#!/usr/bin/env python3
"""Find a returned reader probe and verify its saved installation identities."""
import datetime
import json
from pathlib import Path
import uuid

from build import OUT
from report import summarize, startup
from run import sha

HERE = Path(__file__).resolve().parent


def reader_resources(boot, saved):
    paths = [*saved.glob('*.bmf'), saved / 'zim/wiki.nls']
    result = {}
    for path in paths:
        name = path.relative_to(saved).as_posix()
        expected = sha(path)
        if sha(boot / name) != expected:
            raise ValueError(f'Reader resource changed since installation: {name}')
        result[name] = expected
    return result


def collect(boot, volumes=Path('/Volumes'), destination=HERE, stage=OUT):
    install = json.loads((boot / 'zcal.json').read_text())
    identity = str(uuid.UUID(install['install_id']))
    fixture = stage / ('hardware-fixture-' + identity)
    captured = (fixture / 'inputs.json').exists()
    saved = fixture / 'inputs.json' if captured else stage / 'installed.json'
    inputs = json.loads(saved.read_text())
    # Card metadata is compared with the saved preimage's complete record.
    for key, value in install.items():
        if inputs.get(key) != value:
            raise ValueError(f'Installation identity differs from saved fixture: {key}')
    if captured and sha(fixture / 'card.img') != inputs['fixture_sha256']:
        raise ValueError('Captured starting image changed')
    manifest = inputs['manifest']
    required = {*manifest['files'], *manifest['boot_sha256'], 'init.ini', 'zimlog.on'}
    if set(install['files']) != required:
        raise ValueError('Incomplete installed reader inputs')
    for name, expected in install['files'].items():
        if Path(name).name != name or sha(boot / name) != expected:
            raise ValueError(f'Hardware input differs: {name}')
    for name, expected in {**manifest['files'], **manifest['boot_sha256']}.items():
        if install['files'].get(name) != expected:
            raise ValueError(f'Installed file differs from validated build: {name}')
    for name, expected in install['archives'].items():
        if Path(name).name != name:
            raise ValueError('Invalid archive name')
        candidates = list(volumes.glob('*/' + name))
        if len(candidates) != 1 or candidates[0].stat().st_size != expected['size'] or sha(candidates[0]) != expected['sha256']:
            raise ValueError(f'Hardware archive changed or ambiguous: {name}')
    report, bootlog = boot / 'zimbench.txt', boot / 'zimboot.log'
    phases = summarize(report.read_text(), manifest)
    boot_phases = startup(bootlog.read_text())
    saved_files = inputs.get('verification', {}).get('reader_start_files')
    resources = reader_resources(boot, Path(saved_files)) if saved_files else {}
    date = datetime.date.today().isoformat()
    target = destination / f'device-{date}-{sha(report)[:12]}'
    record = {'date': date, 'install_id': identity, 'manifest': manifest,
              'fixture_sha256': inputs.get('fixture_sha256'),
              'fixture': str(fixture) if captured else None,
              'physical_layout_verified': captured,
              'files': install['files'], 'archives': install['archives'],
              'resources': resources,
              'report': {'file': target.name + '.txt', 'sha256': sha(report)},
              'startup_report': {'file': target.name + '-boot.txt', 'sha256': sha(bootlog)},
              'phases': phases, 'startup': boot_phases, 'hardware_compared': False}
    destination.mkdir(parents=True, exist_ok=True)
    for source, name in ((report, record['report']['file']), (bootlog, record['startup_report']['file'])):
        path = destination / name
        if path.exists() and path.read_bytes() != source.read_bytes():
            raise ValueError(f'Refusing to overwrite existing evidence: {path}')
        path.write_bytes(source.read_bytes())
    (target.with_suffix('.json')).write_text(json.dumps(record, indent=2) + '\n')
    (destination / 'device.json').write_text(json.dumps(record, indent=2) + '\n')
    return target.with_suffix('.json')


def main():
    cards = list(Path('/Volumes').glob('*/zcal.json'))
    if len(cards) != 1:
        raise ValueError(f'Expected one returned reader probe card, found {len(cards)}')
    print(f'Collected verified reader hardware evidence: {collect(cards[0].parent)}')


if __name__ == '__main__':
    main()
