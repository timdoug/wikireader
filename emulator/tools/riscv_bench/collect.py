#!/usr/bin/env python3
"""Find the returned calibration card and validate/preserve its four reports."""
import hashlib
import datetime
import json
from pathlib import Path
import re
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
from score_model import score_riscv


def digest(data):
    return hashlib.sha256(data).hexdigest()


def main():
    cards = list(Path('/Volumes').glob('*/rvcal.json'))
    if len(cards) != 1:
        raise SystemExit(f'Expected one mounted calibration card, found {len(cards)}')
    boot = cards[0].parent
    install = json.loads(cards[0].read_text())
    manifest = install['manifest']
    for name, expected in install['files'].items():
        if digest((boot / name).read_bytes()) != expected:
            raise ValueError(f'Card calibration input changed: {name}')
    reference = (boot / 'rvref.txt').read_text().split()
    reports, text = {}, {}
    date = datetime.date.today().isoformat()
    for variant, row in manifest['variants'].items():
        data = (boot / row['report']).read_bytes()
        report = data.decode()
        got = [word for pair in re.findall(r'^(\w+) (0x[0-9a-f]{8})$', report, re.M)
               for word in pair]
        if got != reference or 'rv32: stopped, reason 1' not in report:
            raise ValueError(f'{variant}: hardware workload did not verify')
        path = HERE / f'device-{date}-{variant}.txt'
        if path.exists() and path.read_bytes() != data:
            raise ValueError(f'Refusing to overwrite a different hardware run: {path}')
        text[variant] = report
        reports[variant] = {'file': path.name, 'sha256': digest(data)}
    score_riscv(text, text)  # complete coverage, console exclusion, all checksums
    for variant, report in text.items():
        (HERE / reports[variant]['file']).write_text(report)
    result = {'date': date, 'manifest': manifest, 'boot_sha256': install['boot_sha256'],
              'reports': reports}
    (HERE / 'device.json').write_text(json.dumps(result, indent=2) + '\n')
    print(f'Collected and verified four reports from {boot}; {HERE}/device.json')


if __name__ == '__main__':
    main()
