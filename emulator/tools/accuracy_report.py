#!/usr/bin/env python3
"""Report per-case timing accuracy and audit the milestone without pooling suites.

Accept one completed score_model output, or multiple outputs from the same
emulator/settings when suites were run separately. Device report hashes are
checked before analysis. Workflow qualification is a separate requirement.
"""
import argparse
import json
import math
from pathlib import Path
import re
import statistics

import score_model as score


def summarize(rows):
    cases = []
    for key, model, device in rows:
        if any(not math.isfinite(v) or v <= 0 for v in (model, device)):
            raise ValueError('timings must be finite and positive')
        signed = model / device - 1
        cases.append({'case': key, 'model': model, 'device': device,
                      'relative_error': signed, 'absolute_relative_error': abs(signed)})
    if not cases:
        raise ValueError('cannot qualify an empty suite')
    errors = sorted(row['absolute_relative_error'] for row in cases)
    inside = sum(e <= .1 + 1e-12 for e in errors)
    large = [row for row in cases if row['absolute_relative_error'] > .2 + 1e-12]
    return {'cases': len(cases), 'median_absolute_relative_error': statistics.median(errors),
            'p90_absolute_relative_error': errors[math.ceil(.9 * len(errors)) - 1],
            'maximum_absolute_relative_error': errors[-1], 'within_10_percent': inside,
            'fraction_within_10_percent': inside / len(cases),
            'representative_threshold_met': inside / len(cases) >= .9,
            'unexplained_over_20_percent': large, 'case_errors': cases}


def table_rows(text, kind):
    if kind == 'mem':
        return score.fields(text, ('case', 'layout', 'method', 'bytes'))
    if kind == 'spi':
        return score.fields(text, ('tx', 'width', 'dst', 'cpu'))
    if kind == 'pipeline':
        return score.pipeline_rows(text)
    rows = {}
    for line in text.splitlines():
        p = line.split()
        if len(p) >= 7 and p[0] == 'UB':
            rows[' '.join(p[1:-5])] = float(p[-1])
    return rows


def collect(runs):
    outputs, identity, device_hashes = {}, None, {}
    for directory in runs:
        result = json.loads((directory / 'scores.json').read_text())
        if not result.get('complete'):
            raise ValueError(f'Calibration output is incomplete: {directory}')
        if score.sha(directory / 'wremu') != result['emulator_sha256']:
            raise ValueError(f'Emulator snapshot changed: {directory}')
        current = (result['emulator_sha256'], result['setting'], result['environment'])
        if identity is not None and identity != current:
            raise ValueError('Cannot merge different emulator binaries or environments')
        identity = current
        for name, digest in result['device_sha256'].items():
            if score.sha(score.ROOT / name) != digest:
                raise ValueError(f'Device evidence changed since the run: {name}')
            device_hashes[name] = digest
        for suite in result['scores']:
            if suite in outputs:
                raise ValueError(f'Duplicate suite: {suite}')
            outputs[suite] = directory
    if set(outputs) != set(score.SUITES):
        raise ValueError(f'Require all five suites, got {sorted(outputs)}')
    reports = {}
    device_paths = {
        'spi': score.ROOT / 'emulator/tools/spi_dma_bench/device-2026-11-05.txt',
        'mem': score.ROOT / 'emulator/tools/mem_dma_bench/device-2026-10-02.txt',
        'ubench': score.UBTOOLS / 'ubench-device.txt',
        'pipeline': score.ROOT / 'emulator/tools/pipeline_bench/device-2026-11-05.txt',
    }
    for kind, filename in [('spi', 'spibench.log'), ('mem', 'membench.log'),
                           ('ubench', 'ubench.txt'), ('pipeline', 'pipeline.log')]:
        model_text = (outputs[kind] / kind / filename).read_text()
        device_text = device_paths[kind].read_text()
        getattr(score, 'score_' + ('mem' if kind == 'mem' else kind))(model_text, device_text)
        m, d = table_rows(model_text, kind), table_rows(device_text, kind)
        score.paired(m, d)
        if kind == 'spi':
            for field, label in [('unit_x100', 'spi-transfer'), ('solo_pass_x100', 'spi-solo')]:
                rows = []
                for key in d:
                    scale = 256 if field == 'solo_pass_x100' and key[-1].startswith('unpack') else 1
                    rows.append((':'.join(key), int(m[key][field]) / 100 / scale,
                                 int(d[key][field]) / 100 / scale))
                reports[label] = summarize(rows)
        elif kind == 'mem':
            reports['memory'] = summarize([(':'.join(k), int(m[k]['median']), int(d[k]['median'])) for k in d])
        elif kind == 'ubench':
            reports[kind] = summarize([(k, m[k], d[k]) for k in d])
        else:
            def time(row):
                return statistics.median(map(int, row['ticks'].split(','))) / int(row['passes'])
            reports[kind] = summarize([(':'.join(k), time(m[k]), time(d[k])) for k in d])
    record = json.loads(score.RISC_DEVICE.read_text())
    model, device = {}, {}
    for variant, row in record['manifest']['variants'].items():
        model[variant] = (outputs['riscv'] / 'riscv' / row['report']).read_text()
        device[variant] = (score.RISC_DEVICE.parent / record['reports'][variant]['file']).read_text()
    score.score_riscv(model, device)
    def rv(text):
        return {m[1]: float(m[2]) for m in re.finditer(r'^rv32: (\w+)\s+\d+ \d+\s+([\d.]+)', text, re.M)}
    rows = []
    for variant in model:
        m, d = rv(model[variant]), rv(device[variant])
        rows.extend((f'{variant}:{k}', m[k], d[k]) for k in d)
    reports['riscv'] = summarize(rows)
    representative = ['spi-transfer', 'spi-solo', 'memory', 'ubench', 'riscv']
    return {'emulator_sha256': identity[0], 'setting': identity[1], 'suites': reports,
            'device_sha256': device_hashes, 'representative_suites': representative,
            'representative_timing_gate_met': all(reports[k]['representative_threshold_met'] for k in representative),
            'all_large_errors_explained': not any(v['unexplained_over_20_percent'] for v in reports.values()),
            'workflow_qualification': {'status': 'unmeasured', 'target_median_relative_error': .05},
            'milestone_complete': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', type=Path, action='append', required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--require-milestone', action='store_true')
    args = parser.parse_args()
    result = collect(args.run)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2) + '\n')
    print('suite            cases median%    p90%    max% within10% unexplained>20%')
    for name, row in result['suites'].items():
        print(f'{name:16} {row["cases"]:5d} {100*row["median_absolute_relative_error"]:7.2f}'
              f' {100*row["p90_absolute_relative_error"]:7.2f}'
              f' {100*row["maximum_absolute_relative_error"]:7.2f}'
              f' {100*row["fraction_within_10_percent"]:9.2f}'
              f' {len(row["unexplained_over_20_percent"]):16d}')
    print(f'Milestone incomplete; detailed case evidence: {args.out}')
    if args.require_milestone:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
