#!/usr/bin/env python3
"""Freeze loop-length predictions before collecting the physical port probe.

This runs a declarative timing model, never guest instructions. Native
execution and input identities come from the completed Grifo-path replay.
"""
import argparse
import datetime
import itertools
import json
from pathlib import Path
import re
import shutil
import statistics
import sys

from frontend import simulate, digest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'emulator/tools/port_bench'))
from run import validate

CONFIGS = {
    'shared_prefix_resource': dict(trailing_ext=True, prefix_gate=True, port_turns=(2, 2), stage_edge=4),
    'no_prefix_resource': dict(trailing_ext=True, prefix_gate=False, port_turns=(2, 2), stage_edge=4),
    'no_fetch_to_data_turn': dict(trailing_ext=True, prefix_gate=True, port_turns=(0, 2), stage_edge=4),
    'no_trailing_prefix': dict(trailing_ext=False, prefix_gate=True, port_turns=(2, 2), stage_edge=4),
    'equivalent_response_split': dict(trailing_ext=True, prefix_gate=True, port_turns=(2, 4),
                                     response_delay=2, stage_edge=4),
}


def body(assembly):
    result = []
    for line in assembly.splitlines():
        opcode, *operands = line.split(None, 1)
        operand = operands[0] if operands else ''
        if opcode == 'ext': result.append('ext')
        elif opcode in ('ld.h', 'ld.w') and '[' in operand:
            result.append('st' if operand.startswith('[') else 'ldh' if opcode == 'ld.h' else 'ldw')
        elif opcode == 'ld.w' and not operand.split(',')[-1].startswith('%'):
            result.append('imm')
        elif opcode in ('ld.w', 'and', 'xor', 'nop'): result.append('alu')
        else: raise ValueError(f'Unsupported timing instruction: {line}')
    return result


def templates(source, manifest):
    result = {}
    for name, count, encoded in re.findall(r'PROBE_N\((\w+),\s*(\d+),\s*("(?:\\.|[^"\\])*")\);', source):
        if name in result: raise ValueError(f'Duplicate assembly template: {name}')
        operations = body(json.loads(encoded))
        probe = manifest['probes'][name]
        if int(count) != probe['unroll'] or 26 + len(operations) * 2 * int(count) != probe['bytes']:
            raise ValueError(f'Assembly layout differs from manifest: {name}')
        result[name] = operations
    if result.keys() != manifest['probes'].keys():
        raise ValueError('Assembly templates do not cover the manifest')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', type=Path, default=ROOT / 'build/wr128/dma-write-review/engine-portq')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    run = json.loads((args.run / 'results.json').read_text())
    manifest = run['manifest']
    stage = ROOT / 'build/wr128/portq'
    source = stage / 'portq.c'
    report = args.run / 'portq/portq.log'
    if (digest(source) != manifest['source_sha256'] or digest(stage / 'portq.app') != manifest['app_sha256'] or
            digest(report) != run['report_sha256'] or digest(args.run / 'wremu') != run['emulator_sha256']):
        raise ValueError('Validated native inputs changed')
    shapes = templates(source.read_text(), manifest)
    rows = validate(report.read_text(), manifest)
    cases = []
    for key, row in rows.items():
        probe = manifest['probes'][key[0]]
        predicted = {}
        for label, config in CONFIGS.items():
            parameters = dict(config, passes=40, unroll=probe['unroll'], offset=int(key[2]))
            if key[3] == 'sdram': parameters.update(shared=False, services=(8, 8, 16))
            value = simulate(shapes[key[0]], **parameters)
            # All configured physical delays are half-clock multiples.
            # Changing numerical resolution must not move any prediction.
            if simulate(shapes[key[0]], quantum=2, tail=0, **parameters) != value:
                raise ValueError(f'Resolution-dependent prediction: {key}, {label}')
            predicted[label] = value
        cases.append({'case': ':'.join(key), 'shape': probe['shape'], 'unroll': probe['unroll'],
                      'code': key[1], 'offset': int(key[2]), 'data': key[3],
                      'emulator_cycles_per_pass': statistics.median(map(int, row['ticks'].split(','))) / int(row['passes']),
                      'predicted_cycles_per_pass': predicted})
    contrasts = []
    for first, second in itertools.combinations(CONFIGS, 2):
        for data in manifest['data']:
            pairs = [(row['case'], row['predicted_cycles_per_pass'][first] - row['predicted_cycles_per_pass'][second])
                     for row in cases if row['data'] == data]
            contrasts.append({'first': first, 'second': second, 'data': data,
                              'differing_cases': sum(delta != 0 for _, delta in pairs),
                              'maximum_separation_cycles_per_pass': max(abs(delta) for _, delta in pairs),
                              'largest_separations': sorted(pairs, key=lambda pair: abs(pair[1]), reverse=True)[:6]})
    result = {'experiment_only': True, 'hardware_collected': False,
              'created_at': datetime.datetime.now(datetime.timezone.utc).isoformat(),
              'scope': 'Fixed prospective steady-loop predictions; no hardware data or fitted timer overhead',
              'manifest': manifest, 'configs': CONFIGS, 'cases': cases, 'contrasts': contrasts,
              'native_emulator_sha256': run['emulator_sha256'], 'native_report_sha256': digest(report),
              'frontend_sha256': digest(Path(__file__).with_name('frontend.py')),
              'prediction_script_sha256': digest(Path(__file__)),
              'limitations': ['No architectural execution, SDRAM queues or DMA in the timing hypotheses',
                              'Numerical checks do not establish physical bus structure',
                              'Timing-equivalent configurations cannot be distinguished by this probe']}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    # Never silently replace a prospective prediction after seeing silicon.
    with args.out.open('x') as stream: stream.write(json.dumps(result, indent=2) + '\n')
    shutil.copy2(Path(__file__).with_name('frontend.py'), args.out.with_suffix('.frontend.py'))
    shutil.copy2(Path(__file__), args.out.with_suffix('.generator.py'))
    print(f'Frozen {len(cases)} cases for {len(CONFIGS)} hypotheses: {args.out}')
    for row in contrasts:
        if row['first'] == 'shared_prefix_resource':
            print(f'{row["second"]:28} {row["data"]:5} differs in {row["differing_cases"]:3} cases; '
                  f'max separation {row["maximum_separation_cycles_per_pass"]:5.2f} clocks/pass')


if __name__ == '__main__':
    main()
