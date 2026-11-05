#!/usr/bin/env python3
"""Compare a physical return with frozen, independently identified forecasts."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import statistics

from report import KEYS, validate

ROOT = Path(__file__).resolve().parents[3]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def comparison(model, hardware, manifest):
    mh, mr = validate(model, manifest)
    dh, dr = validate(hardware, manifest)
    # REF.D25 is live, read-only SELDO status (manual II.4.1.5/II.4.4),
    # not a timing configuration bit. Preserve its difference explicitly;
    # every writable setting and the timer/address contexts must match.
    mc, dc = dict(mh), dict(dh)
    mc['refresh'] = f'{int(mc["refresh"], 16) & ~(1 << 25):08x}'
    dc['refresh'] = f'{int(dc["refresh"], 16) & ~(1 << 25):08x}'
    if mc != dc:
        raise ValueError('Hardware and model clock/address contexts differ')
    rows, groups = [], {}
    for key in sorted(dr):
        samples = {name: list(map(int, data[key]['ticks'].split(',')))
                   for name, data in [('model', mr), ('device', dr)]}
        medians = {name: statistics.median(values) for name, values in samples.items()}
        row = dict(key=dict(zip(KEYS, key)), samples=samples, median_ticks=medians,
                   signed_error=medians['model'] / medians['device'] - 1)
        rows.append(row)
        groups.setdefault((key[2], key[5], key[6]), []).append(row)
    summaries = []
    for (code, iqb, passes), values in sorted(groups.items()):
        errors = sorted(abs(v['signed_error']) for v in values)
        summaries.append(dict(code=code, iqb=int(iqb), passes=int(passes), cases=len(values),
                              within10=sum(e <= .1 for e in errors), median=statistics.median(errors),
                              p90=errors[math.ceil(len(errors) * .9) - 1], maximum=max(errors)))
    return dict(cases=rows, groups=summaries, raw_elapsed_ticks=True,
                raw_context=dict(model=mh, device=dh),
                self_refresh_status=dict(model=bool(int(mh['refresh'], 16) & (1 << 25)),
                                         device=bool(int(dh['refresh'], 16) & (1 << 25))),
                fitted_offset_subtracted=False, scope='Controlled CPU/SDRAM fetch timing; not whole-reader qualification')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--predictions', type=Path, default=ROOT / 'build/wr128/fetchq/prospective.json')
    parser.add_argument('--device', type=Path, default=Path(__file__).with_name('device.json'))
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    predictions = json.loads(args.predictions.read_text())
    device = json.loads(args.device.read_text())
    if not device.get('valid') or device['manifest'] != predictions['manifest']:
        raise ValueError('No verified matching physical return')
    report = args.device.parent / device['report']['file']
    if sha(report) != device['report']['sha256']:
        raise ValueError('Physical raw report changed')
    manifest = predictions['manifest']
    for card_name, model_name in [('fetchq.app', 'bench.app'), ('fetchq.ico', 'bench.ico'),
                                  ('kernel.elf', 'kernel.elf'), ('init.app', 'init.app')]:
        if device['inputs'][card_name] != predictions['inputs'][model_name]:
            raise ValueError(f'Physical boot/probe input differs: {card_name}')
    results = {}
    for setting, forecast in predictions['models'].items():
        run = ROOT / forecast['run']
        model = run / 'fetchq/fetchq.log'
        if sha(model) != forecast['report_sha256'] or sha(run / 'wremu') != forecast['emulator_sha256']:
            raise ValueError('Frozen native forecast changed')
        results[setting] = comparison(model.read_text(), report.read_text(), manifest)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(dict(complete=True, prediction_sha256=sha(args.predictions),
                                       hardware_sha256=sha(report), results=results), indent=2) + '\n')
    print(f'Compared all 600 cases separately by code/IQB/run length: {args.out}')


if __name__ == '__main__':
    main()
