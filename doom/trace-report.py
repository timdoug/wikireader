#!/usr/bin/env python3
"""Summarize card-side Doom timing; optionally compare a matched emulator log."""
import argparse
import json
from pathlib import Path
import shlex


def fields(line):
    result = {}
    for token in shlex.split(line)[1:]:
        if '=' in token:
            key, value = token.split('=', 1)
            try:
                value = int(value)
            except ValueError:
                pass
            result[key] = value
    return result


def summarize(path):
    runs, run, pending = [], None, None
    for line in Path(path).read_text().splitlines():
        if line.startswith('DOOMPERF '):
            run = dict(header=fields(line), boot=[], windows=[], flush_us=[])
            runs.append(run)
            pending = None
        elif run is not None:
            if line.startswith('BOOT '):
                run['boot'].append(fields(line))
            elif line.startswith(('BENCH ', 'WINDOW ', 'WARMUP ', 'PARTIAL ')):
                pending = dict(kind=line.split()[0], **fields(line))
                run['windows'].append(pending)
            elif line.startswith('STATE ') and pending is not None:
                pending['state'] = fields(line)
            elif line.startswith('IO ') and pending is not None:
                pending['io'] = fields(line)
            elif line.startswith('FLUSH '):
                run['flush_us'].append(fields(line)['write_close_us'])
            elif line == 'BENCH_DONE':
                run['benchmark_complete'] = True
    for run in runs:
        for w in run['windows']:
            n, elapsed = w['frames'], w['elapsed_us']
            if not n or not elapsed:
                continue
            w['fps'] = n * 1000000 / elapsed
            w['average_ms'] = {name: w[name + '_us'] / n / 1000
                               for name in ('engine', 'lcd', 'bsp', 'planes', 'masked')}
            state = w.get('state', {})
            expected = dict(episode=1, map=1, skill=2, detail=1, width=160, height=168)
            w['comparable'] = (w['kind'] == 'BENCH' and run.get('benchmark_complete', False)
                and all(state.get(k) == v for k, v in expected.items())
                and w['level'] == n and all(w[k] == 0 for k in ('menu', 'demo', 'wipe', 'moved', 'changed'))
                and sum(map(int, str(w['bins']).split(','))) == n
                and elapsed >= 10000000 and 'io' in w
                and all(w['io'][k] == 0 for k in ('read_errors', 'timeouts', 'dma_errors')))
            if state:
                w['tics_per_second'] = (state['tic_end'] - state['tic_start']) * 1000000 / elapsed
    return runs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('--compare', type=Path, help='doomperf.log from the emulator')
    args = parser.parse_args()
    runs = summarize(args.log)
    result = dict(runs=runs)
    if args.compare:
        reference = summarize(args.compare)
        comparisons = []
        for run in runs:
            for w in run['windows']:
                if not w.get('comparable'):
                    continue
                matches = [b for r in reference if r['header']['build'] == run['header']['build']
                           for b in r['windows'] if b.get('comparable')]
                if not matches:
                    raise ValueError('No complete reference benchmark with the same source build ID')
                b = matches[-1]
                if any(w['state'][k] != b['state'][k] for k in ('x', 'y', 'angle', 'health')):
                    raise ValueError('Reference scene/player state differs')
                comparisons.append(dict(build=run['header']['build'], measured_fps=w['fps'],
                    emulator_fps=b['fps'], measured_over_emulator=w['fps'] / b['fps'],
                    phase_ratio={k: w['average_ms'][k] / v for k, v in b['average_ms'].items() if v}))
        if not comparisons:
            raise ValueError('No completed, stationary benchmark in the supplied log')
        result['comparisons'] = comparisons
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
