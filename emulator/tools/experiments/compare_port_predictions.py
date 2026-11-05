#!/usr/bin/env python3
"""Evaluate the unchanged, pre-collection local-port forecasts against hardware.

This reads frozen values; it never reruns or refits a timing model. Executing
CPU qualification belongs to the separately identified port_bench runs.
"""
import argparse
import hashlib
import json
from pathlib import Path
import statistics
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'emulator/tools/port_bench'))
from run import validate
sys.path.insert(0, str(ROOT / 'emulator/tools'))
from accuracy_report import summarize

EXPECTED = {
    'port-prospective.json': '51ffc69601f6b3108824263fa1c75f019834096b9f4ecdb474382c8a435ed397',
    'port-prospective-queues.json': '82be824eb502871b6796dbaa7688c914758b8ae8d07fb452143bd8d621cdbdda',
}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device', type=Path, default=ROOT / 'emulator/tools/port_bench/device.json')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    device = json.loads(args.device.read_text())
    if not device.get('valid') or device['cases'] != 560:
        raise ValueError('No complete, matching physical local-port return')
    report = args.device.parent / device['report']['file']
    if sha(report) != device['report']['sha256']:
        raise ValueError('Physical report changed')
    native = validate(report.read_text(), device['manifest'])
    times = {':'.join(key): statistics.median(map(int, row['ticks'].split(','))) / int(row['passes'])
             for key, row in native.items()}
    forecasts = {}
    for filename, expected in EXPECTED.items():
        path = ROOT / 'build/wr128/frontend-lab' / filename
        if sha(path) != expected:
            raise ValueError(f'Prospective predictions changed after collection: {filename}')
        forecast = json.loads(path.read_text())
        if (forecast['manifest'] != device['manifest'] or forecast['hardware_collected'] or
                len(forecast['cases']) != 560 or {r['case'] for r in forecast['cases']} != times.keys()):
            raise ValueError('Frozen forecast workload differs from hardware')
        if (sha(path.with_suffix('.frontend.py')) != forecast['frontend_sha256'] or
                sha(path.with_suffix('.generator.py')) != forecast['prediction_script_sha256']):
            raise ValueError('Frozen model/generator source changed')
        forecasts[filename] = forecast
    original, queues = (forecasts[name] for name in EXPECTED)
    original_rows = {r['case']: r for r in original['cases']}
    for row in queues['cases']:
        for label in original['configs']:
            if row['predicted_cycles_per_pass'][label] != original_rows[row['case']]['predicted_cycles_per_pass'][label]:
                raise ValueError('Original predictions were modified in the queue-capacity snapshot')
    results = {}
    for label in queues['configs']:
        def stats(rows):
            return summarize([(r['case'], r['predicted_cycles_per_pass'][label], times[r['case']]) for r in rows])
        results[label] = dict(all=stats(queues['cases']),
            by_data={data: stats([r for r in queues['cases'] if r['data'] == data]) for data in ['a0', 'sdram']},
            by_unroll={str(count): stats([r for r in queues['cases'] if r['unroll'] == count])
                       for count in [1, 2, 3, 4, 8, 12, 16]})
    contrasts = []
    for width in ('h', 'w'):
        for count in (1, 2, 3, 4, 8, 12, 16):
            for offset in ('0', '2'):
                for data in ('a0', 'sdram'):
                    dep = native[(f'{width}_xor_dep_u{count}', 'a0', offset, data)]
                    ind = native[(f'{width}_xor_indep_u{count}', 'a0', offset, data)]
                    a, b = [list(map(int, r['ticks'].split(','))) for r in (dep, ind)]
                    scale = 20000 * count
                    contrasts.append(dict(width=width, unroll=count, offset=int(offset), data=data,
                        dependent_ticks=a, independent_ticks=b,
                        median_cycles_per_pair=(statistics.median(a)-statistics.median(b))/scale,
                        descriptive_min_cycles_per_pair=(min(a)-max(b))/scale,
                        descriptive_max_cycles_per_pair=(max(a)-min(b))/scale))
    result = dict(complete=True, hardware_report_sha256=sha(report),
        frozen_sha256=EXPECTED, cases=560, configurations=results, matched_dependency_contrasts=contrasts,
        model_refitted=False, prospective_predictions_overwritten=False,
        scope='Prospective declarative-model assessment; executing native CPU results are recorded separately',
        interpretation='Report shared-A0 and separate-port cases independently. No fitted timer subtraction. The hardware holdout still misses the 90 percent target for shared A0.')
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2) + '\n')
    for label, row in results.items():
        print(label, f'{row["all"]["within_10_percent"]}/560 within 10%; shared A0 '
              f'{row["by_data"]["a0"]["within_10_percent"]}/280; separate port '
              f'{row["by_data"]["sdram"]["within_10_percent"]}/280')
    print(f'56 matched dependency contrasts; max median separation '
          f'{max(abs(c["median_cycles_per_pair"]) for c in contrasts):.7f} clocks/pair')


if __name__ == '__main__':
    main()
