#!/usr/bin/env python3
"""Timing-only C33 frontend experiment, not an executing CPU or qualification.

Use the fixed instruction shapes from pipeline.c to test fetch/decode and
memory backpressure hypotheses against all 160 A0-code/A0-data cases.
No guest instructions, firmware or device I/O are executed by this script.
"""
from collections import deque
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import statistics
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from score_model import pipeline_rows
from accuracy_report import summarize

BODIES={
 'base':['alu'], 'ext1':['ext','alu'], 'ext2':['ext','ext','alu'],
 'ldh':['ldh'], 'ldw':['ldw'], 'store':['st'],
 'ext1_load':['ext','ldw'], 'ext2_load':['ext','ext','ldw'],
 'ext1_store':['ext','st'], 'ext2_store':['ext','ext','st'],
 'load_ext':['ldh','ext','alu'], 'move_ext':['alu','ext','alu'],
 'load_use0':['ldh','alu'], 'load_use1':['ldh','alu','alu'],
 'load_use2':['ldh','alu','alu','alu'],
 'word_use0':['ldw','alu'], 'word_use1':['ldw','alu','alu'],
 'ext1_branch':['alu','ext','branch'],
 'ext2_branch':['alu','ext','ext','branch'],
 'mixed':['ext','ext','imm','ldh','ext','alu','alu'],
}

def simulate(body, offset=0, depth=4, tail=1, services=(6, 8, 4),
             buffer=1, passes=160, freeze=0, shared=True,
             trailing_ext=False, events=None, port_turns=None, port_events=None,
             stage_edge=1, response_delay=0, quantum=1, prefix_gate=False, unroll=8):
    """Return steady loop cycles/pass using quarter-MCLK event scheduling.

The hypotheses are a shared 32-bit A0 port, four raw halfwords, one
completed decode slot, and manual three-cycle branch execution.
Memory E-stage services (1.5/2/1 MCLK) come from SDRAM-code/A0-data
slopes; their applicability with A0 code is under test here.
freeze: 0 allows overlap, 1 blocks decode throughout memory execution,
2 allows decode during the last memory execution cycle.
trailing_ext optionally tests decoding a queued EXT alongside a preceding
unextended register operation. This is a hypothesis about extension-latch
availability, not an established property or an architectural executor.
port_turns optionally separates fetch-to-data and data-to-fetch port
turnaround (quarter clocks) from the legacy tail after every data access.
stage_edge constrains F/E starts to quarter-, half- or full-MCLK edges.
response_delay separates data-result latency from RAM-port occupancy.
quantum changes numerical resolution, not a modeled hardware latency.
prefix_gate tests extension decoding sharing a resource with local data E.
unroll is the number of body copies before the decrement and branch.
This does not execute instruction semantics or model dependencies."""
    if (depth < 2 or buffer < 1 or passes < 4 or min(services) < 1 or tail < 0 or
            not isinstance(unroll, int) or unroll < 1 or not body):
        raise ValueError('invalid frontend experiment configuration')
    if (response_delay < 0 or response_delay >= min(services) or
            stage_edge not in (1, 2, 4)) or (port_turns is not None and
            (len(port_turns) != 2 or min(port_turns) < 0)):
        raise ValueError('invalid clock edge or port turnaround')
    delays = [*services, response_delay, *(port_turns if port_turns is not None else (tail,))]
    if quantum not in (1, 2) or any(delay % quantum for delay in delays):
        raise ValueError('numerical resolution must divide the modeled delays')
    code = body * unroll + ['alu', 'loop']
    base = offset
    raw_queue = deque()
    fetch_address = base & ~3
    port_free = 0
    port_owner = None
    pending_data = None
    execute = None
    decode = None
    decoded = deque()
    prefix_count = 0
    pc = base
    quarter = 0
    loop_ends = []
    last_decode_start = -4

    def op(addr):
        index = (addr - base) // 2
        return code[index] if 0 <= index < len(code) else 'alu'

    def offer_fetch():
        nonlocal fetch_address, port_free, port_owner
        fetch_ready = port_free
        if port_turns is not None and port_owner == 'data':
            fetch_ready += port_turns[1]
        if (fetch_ready <= quarter and len(raw_queue) <= depth - 2 and
                not (shared and pending_data and pending_data[0] <= quarter)):
            if port_events is not None:
                port_events.append({'owner': 'fetch', 'quarter': quarter, 'end': quarter + 4,
                                    'code_address': fetch_address, 'kind': 'fetch'})
            for address in (fetch_address, fetch_address + 2):
                if address >= pc:
                    raw_queue.append((address, op(address), quarter + 4))
            fetch_address += 4
            port_free = quarter + 4
            port_owner = 'fetch'
    for iteration in range(2000000):
        assert len(raw_queue) <= depth and len(decoded) <= buffer
        if execute and execute[2] is not None and (execute[2] <= quarter):
            kind, address, completed_at = execute
            execute = None
            if kind in ('branch', 'loop'):
                target = base if kind == 'loop' else address + 2
                raw_queue.clear()
                fetch_address = target & ~3
                pc = target
                prefix_count = 0
                decode = None
                decoded.clear()
                last_decode_start = quarter - 4
                if kind == 'loop':
                    loop_ends.append(quarter)
                    if len(loop_ends) == passes:
                        return (loop_ends[-1] - loop_ends[passes // 2 - 1]) / (passes - passes // 2) / 4
        if decode and decode[0] <= quarter:
            end, kind, address, prefixes, following = decode
            decode = None
            if kind == 'ext':
                prefix_count += 1
            else:
                decoded.append((kind, address, prefixes))
                prefix_count = following
        if execute is None and decoded and quarter % stage_edge == 0:
            kind, address, prefixes = decoded.popleft()
            if events is not None:
                events.append({'quarter': quarter, 'loop': len(loop_ends),
                               'kind': kind, 'address': address, 'prefixes': prefixes})
            execute = [kind, address, None]
            if kind in ('ldh', 'ldw', 'st'):
                cost = services[('ldh', 'ldw', 'st').index(kind)]
                pending_data = (quarter + (4 if prefixes else 0), cost)
            else:
                execute[2] = quarter + (12 if kind in ('branch', 'loop') else 4)
        # Dispatch can issue a zero-delay data request at this same instant.
        # Servicing it before dispatch inserted an artificial quarter-clock
        # per access, even with a separate, entirely idle memory port.
        data_ready = port_free
        if port_turns is not None and port_owner == 'fetch':
            data_ready += port_turns[0]
        if pending_data and pending_data[0] <= quarter and (not shared or data_ready <= quarter):
            request, cost = pending_data
            pending_data = None
            execute[2] = quarter + cost
            if shared:
                if port_events is not None:
                    port_events.append({'owner': 'data', 'quarter': quarter, 'end': quarter + cost - response_delay,
                                        'ready': quarter + cost,
                                        'code_address': execute[1], 'kind': execute[0]})
                port_free = quarter + cost - response_delay + (tail if port_turns is None else 0)
                port_owner = 'data'
        offer_fetch()
        blocked = execute is not None and execute[0] in ('ldh', 'ldw', 'st')
        if freeze == 2 and blocked:
            blocked = execute[2] is None or execute[2] > quarter + 4
        if (quarter % stage_edge == 0 and decode is None and len(decoded) < buffer and
                quarter >= last_decode_start + 4 and raw_queue and
                not (freeze and blocked) and
                not (prefix_gate and shared and blocked and raw_queue[0][1] == 'ext')):
            address, kind, ready = raw_queue[0]
            if ready <= quarter + 4:
                consume = 1
                following = 0
                prefixes = prefix_count
                if (kind == 'ext' and ready <= quarter and len(raw_queue) > 1 and
                        raw_queue[1][1] != 'ext' and raw_queue[1][2] <= quarter + 4):
                    consume = 2
                    kind = raw_queue[1][1]
                    address = raw_queue[1][0]
                    prefixes += 1
                # Hypothesis: an unextended register operation leaves the
                # extension latch available for lookahead into a following
                # EXT that is already in the instruction queue. Capture it
                # for the NEXT operation, never the current one. An extended
                # operation keeps that latch occupied and cannot do this.
                if (not (prefix_gate and shared and blocked) and
                        trailing_ext and kind in ('alu', 'imm') and prefixes == 0 and consume == 1 and
                        len(raw_queue) > 1 and raw_queue[1][1] == 'ext' and
                        raw_queue[1][2] <= quarter):
                    consume = 2
                    following = 1
                for _ in range(consume):
                    raw_queue.popleft()
                pc = address + 2 + 2 * following
                decode = (quarter + 4, kind, address, prefixes, following)
                last_decode_start = quarter
        # Decode may just have freed FIFO space. Offer that space to the
        # fetcher at this same instant, rather than adding one numerical
        # tick of undocumented refill latency.
        offer_fetch()
        quarter += quantum
    raise RuntimeError('simulation did not finish')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def bus_compare(config, record_path):
    """Check all A0 placements against the returned, frozen bus application.

    Extract instruction categories from the hash-verified generated source,
    rather than relabeling controls by their results or dependency names.
    This remains a timing-only model; no guest instructions are executed.
    """
    record = json.loads(record_path.read_text())
    manifest = record['manifest']
    log = record_path.parent / record['report']['file']
    source = Path(__file__).resolve().parents[3] / 'build/wr128/buspipe/buspipe.c'
    if digest(log) != record['report']['sha256'] or digest(source) != manifest['source_sha256']:
        raise ValueError('Frozen bus report or instruction source changed')
    spec = importlib.util.spec_from_file_location('bus_report_validation', record_path.parent / 'run.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    rows = module.validate(log.read_text(), manifest)
    bodies = {}
    pattern = r'PROBE\((\w+),\s*((?:"(?:\\.|[^"\\])*"\s*)+)\);'
    for name, strings in re.findall(pattern, source.read_text()):
        asm = ''.join(json.loads(s) for s in re.findall(r'"(?:\\.|[^"\\])*"', strings))
        body = []
        for line in asm.splitlines():
            if line.endswith(':'):
                continue
            op = line.split()[0]
            if op == 'ext':
                body.append('ext')
            elif op == 'jreq':
                body.append('branch')
            elif op in ('ld.h', 'ld.w') and '[' in line:
                body.append('st' if line.split(None, 1)[1].startswith('[') else
                            'ldh' if op == 'ld.h' else 'ldw')
            elif op in ('ld.w', 'and', 'xor', 'cmp'):
                body.append('alu')
            else:
                raise ValueError(f'Unsupported timing instruction: {line}')
        bodies[name] = body
    if bodies.keys() != manifest['probes'].keys():
        raise ValueError('Instruction shapes do not cover the frozen bus manifest')
    results = []
    for (name, code, offset, data), row in rows.items():
        if code != 'a0':
            continue
        # Separate SDRAM data from the local instruction port. Two-cycle
        # reads and four-cycle posted writes are explicit hypotheses here;
        # this experiment has no SDRAM queue, refresh or arbitration model.
        parameters = dict(config)
        if data == 'sdram':
            parameters.update(shared=False, services=(8, 8, 16))
        measured = statistics.median(map(int, row['ticks'].split(','))) / int(row['passes'])
        results.append({'case': f'{name}:{offset}:{data}', 'probe': name,
                        'offset': int(offset), 'data': data,
                        'predicted_cycles_per_pass': simulate(bodies[name], offset=int(offset), **parameters),
                        'device_cycles_per_pass': measured})
    groups = {}
    for data in manifest['data']:
        groups[data] = summarize([(r['case'], r['predicted_cycles_per_pass'], r['device_cycles_per_pass'])
                                  for r in results if r['data'] == data])
    return {'hardware_sha256': digest(log), 'instruction_source_sha256': digest(source),
            'accuracy_by_data': groups, 'cases': results,
            'limitations': ['SDRAM data service is a constant timing hypothesis',
                            'No SDRAM queues, refresh, DMA or interrupt model',
                            'Neither instruction semantics nor result dependencies are simulated']}


def compare(config, report):
    record = json.loads(report.read_text())
    path = report.parent / record['log']
    if digest(path) != record['device_log_sha256']:
        raise ValueError('Hardware pipeline report changed')
    device = pipeline_rows(path.read_text())
    results = []
    for name, body in BODIES.items():
        for offset in range(0, 16, 2):
            row = device[(name, 'a0', str(offset), 'a0')]
            ticks = list(map(int, row['ticks'].split(',')))
            if len(ticks) != 3 or min(ticks) <= 0 or int(row['passes']) != 20000:
                raise ValueError('Incomplete or mismatched timing samples')
            measured = statistics.median(ticks) / int(row['passes'])
            predicted = simulate(body, offset=offset, **config)
            results.append({'case': f'{name}:{offset}', 'probe': name, 'offset': offset,
                            'predicted_cycles_per_pass': predicted,
                            'device_cycles_per_pass': measured})
    return {'experiment_only': True, 'config': config,
            'hardware_sha256': digest(path), 'script_sha256': digest(Path(__file__)),
            'scope': 'A0 code and A0 data; steady loop timing excludes timer/entry overhead',
            'limitations': ['No architectural execution or load-result dependency model',
                            'No SDRAM, peripheral, DMA or interrupt scheduling',
                            'Shared-port and decoder-backpressure rules are hypotheses'],
            'accuracy': summarize([(r['case'], r['predicted_cycles_per_pass'],
                                    r['device_cycles_per_pass']) for r in results]),
            'cases': results}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device', type=Path, default=Path(__file__).resolve().parents[1] /
                        'pipeline_bench/device-2026-11-05.json')
    parser.add_argument('--freeze-decode', choices=['none', 'memory', 'wait'], default='none')
    parser.add_argument('--depth', type=int, default=4)
    parser.add_argument('--tail', type=int, default=1, help='data port handoff in quarter MCLKs')
    parser.add_argument('--passes', type=int, default=160)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--bus-device', type=Path, help='also check the returned matched bus controls')
    parser.add_argument('--trailing-ext', action='store_true', help='test lookahead past an unextended register operation')
    parser.add_argument('--port-turns', help='fetch-to-data,data-to-fetch handoff in quarter MCLKs')
    parser.add_argument('--stage-edge', choices=['quarter', 'half', 'clock'], default='quarter')
    parser.add_argument('--response-delay', type=int, default=0, help='result latency after releasing the port, in quarter MCLKs')
    parser.add_argument('--prefix-gate', action='store_true', help='test extension-latch contention during local data execution')
    args = parser.parse_args()
    config = {'depth': args.depth, 'tail': args.tail, 'passes': args.passes,
              'freeze': ['none', 'memory', 'wait'].index(args.freeze_decode),
              'trailing_ext': args.trailing_ext,
              'port_turns': tuple(map(int, args.port_turns.split(','))) if args.port_turns else None,
              'stage_edge': {'quarter': 1, 'half': 2, 'clock': 4}[args.stage_edge],
              'response_delay': args.response_delay, 'prefix_gate': args.prefix_gate}
    result = compare(config, args.device)
    if args.bus_device:
        result['bus'] = bus_compare(config, args.bus_device)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2) + '\n')
    row = result['accuracy']
    print(f"{row['cases']} A0 cases; median error {100*row['median_absolute_relative_error']:.2f}%, "
          f"p90 {100*row['p90_absolute_relative_error']:.2f}%, "
          f"max {100*row['maximum_absolute_relative_error']:.2f}%")
    if 'bus' in result:
        for data, row in result['bus']['accuracy_by_data'].items():
            print(f"Bus controls, {data} data: {row['within_10_percent']}/{row['cases']} within 10%, "
                  f"median {100*row['median_absolute_relative_error']:.2f}%, "
                  f"p90 {100*row['p90_absolute_relative_error']:.2f}%, "
                  f"max {100*row['maximum_absolute_relative_error']:.2f}%")
    print(f'Experimental evidence only: {args.out}')


if __name__ == '__main__':
    main()
