#!/usr/bin/env python3
"""Score timing against recorded device suites through FLASH/Grifo.

Build the benchmark apps first (see emulator/README.md). Each invocation
keeps its own cards, console logs, guest reports and hashes in --out.
No attached card or hardware is needed. WREMU_MODEL and --set compose.
"""
import argparse
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
UBTOOLS = ROOT / 'nuttx/overlay/nuttx/boards/c33/s1c33e07/wikireader/tools'
VARIANTS = dict(sdram='rvsdr', code='rva0', **{'code+state': 'rva0s', 'asm': 'rvbench'})
KERNELS = 'alu branch mul div load store copy bytes crc sieve'.split()
SUITES = ('spi', 'mem', 'ubench', 'riscv', 'pipeline')
RISC_DEVICE = ROOT / 'emulator/tools/riscv_bench/device.json'


def fat_helper():
    spec = importlib.util.spec_from_file_location(
        'score_fat', ROOT / 'emulator/tools/mem_dma_bench/run.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def fields(text, key):
    rows = {}
    for line in text.splitlines():
        if line.startswith('RESULT '):
            row = dict(re.findall(r'(\w+)=(\S+)', line))
            identity = tuple(row[k] for k in key)
            if identity in rows:
                raise ValueError(f'duplicate result {identity}')
            rows[identity] = row
    return rows


def paired(model, device):
    if model.keys() != device.keys() or not device:
        raise ValueError(f'result coverage differs: missing {device.keys() - model.keys()}, '
                         f'extra {model.keys() - device.keys()}')
    return [(model[k], device[k]) for k in device]


def rms(values):
    return math.sqrt(sum(x*x for x in values) / len(values))


def log_score(model, device):
    pairs = paired(model, device)
    if any(not math.isfinite(v) or v <= 0 for pair in pairs for v in pair):
        raise ValueError('timings must be finite and positive')
    return rms([math.log(m / d) for m, d in pairs])


def score_spi(model, device):
    def parse(text):
        rows = fields(text, ('tx', 'width', 'dst', 'cpu'))
        end = re.search(r'END SPIBENCH cases=(\d+) failures=0\b', text)
        if not end or len(rows) != int(end[1]) or any(r['ok'] != '1' for r in rows.values()):
            raise ValueError('incomplete or failed spibench')
        return rows
    pairs = paired(parse(model), parse(device))
    # A kernel block is 256 word passes; compare it in the same units as
    # the short loops. DMA loss is concurrent minus solo cycles per pass.
    solo, loss, transfer = [], [], []
    for m, d in pairs:
        scale = 256 if m['cpu'].startswith('unpack') else 1
        ms, ds = (int(r['solo_pass_x100']) / 100 / scale for r in (m, d))
        mp, dp = (int(r['pass_x100']) / 100 / scale for r in (m, d))
        solo.append(ms - ds)
        loss.append((mp - ms) - (dp - ds))
        transfer.append(math.log(int(m['unit_x100']) / int(d['unit_x100'])))
    return {'cases': len(pairs), 'solo_cycles_rms': rms(solo),
            'dma_loss_cycles_rms': rms(loss), 'transfer_log_rms': rms(transfer)}


def score_mem(model, device):
    def parse(text):
        rows = fields(text, ('case', 'layout', 'method', 'bytes'))
        if ('complete=1 state_restored=1' not in text or 'failed=0' not in text
                or len(rows) != 120 or any(r['verified'] != '1' for r in rows.values())):
            raise ValueError('incomplete or failed membench')
        return {k: int(r['median']) for k, r in rows.items()}
    m, d = parse(model), parse(device)
    return {'cases': len(d), 'log_rms': log_score(m, d)}


def score_ubench(model, device):
    def parse(text):
        result = {}
        for line in text.splitlines():
            parts = line.split()
            if len(parts) >= 7 and parts[0] == 'UB':
                name = ' '.join(parts[1:-5])
                if name in result:
                    raise ValueError(f'duplicate ubench loop {name}')
                result[name] = (tuple(parts[-5:-3]), float(parts[-1]))
        return result
    m, d = parse(model), parse(device)
    for mr, dr in paired(m, d):
        if mr[0] != dr[0]:
            raise ValueError('ubench code size or instruction count differs')
    return {'cases': len(d), 'log_rms': log_score({k: v[1] for k, v in m.items()},
                                               {k: v[1] for k, v in d.items()})}


def score_riscv(model, device):
    def parse(text):
        if 'rv32: console output:' not in text or 'truncated' in text:
            raise ValueError('RISC-V calibration must exclude console time and contain a complete report')
        matches = list(re.finditer(r'^rv32: (\w+)\s+\d+ \d+\s+([\d.]+)', text, re.M))
        rows = {m[1]: float(m[2]) for m in matches}
        if len(matches) != len(rows):
            raise ValueError('duplicate RISC-V kernel')
        if set(rows) != set(KERNELS) or 'rv32: total' not in text:
            raise ValueError('incomplete RISC-V report')
        counts = {m[1]: int(m[2]) for m in re.finditer(r'^rv32: (\w+)\s+(\d+) \d+', text, re.M)}
        checksums = dict(re.findall(r'^(\w+) (0x[0-9a-f]{8})$', text, re.M))
        if set(checksums) != set(KERNELS):
            raise ValueError('RISC-V calibration must save every guest checksum')
        return rows, counts, checksums
    m, d = {}, {}
    for variant in VARIANTS:
        mr, mc, ms = parse(model[variant])
        dr, dc, ds = parse(device[variant])
        if mc != dc or ms != ds:
            raise ValueError('RISC-V workload counts or checksums differ')
        m.update({(variant, k): v for k, v in mr.items()})
        d.update({(variant, k): v for k, v in dr.items()})
    return {'cases': len(d), 'log_rms': log_score(m, d)}


def pipeline_rows(text):
    rows = fields(text, ('probe', 'code', 'offset', 'data'))
    end = re.search(r'END PIPELINE cases=(\d+) failures=0\b', text)
    if (not end or len(rows) != int(end[1]) or len(rows) != 360 or
            any(r['ok'] != '1' for r in rows.values())):
        raise ValueError('pipeline probe incomplete or failed')
    return rows


def score_pipeline(model, device):
    import statistics

    m, d = pipeline_rows(model), pipeline_rows(device)
    for mr, dr in paired(m, d):
        if any(mr[field] != dr[field] for field in ('bytes', 'passes', 'result')):
            raise ValueError('pipeline probe workloads differ')
    def times(rows):
        return {k: statistics.median(map(int, r['ticks'].split(','))) / int(r['passes'])
                for k, r in rows.items()}
    return {'cases': len(d), 'log_rms': log_score(times(m), times(d))}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class Runner:
    def __init__(self, args, env):
        self.args, self.env, self.fat = args, env, fat_helper()
        self.hashes = {}
        # A rebuild during a long comparison must not mix emulator versions.
        snapshot = args.out / 'wremu'
        shutil.copy2(args.wremu, snapshot)
        args.wremu = snapshot

    def run(self, name, app, icon, arguments, report, extra=None, uart=None, keep_card=False):
        out = self.args.out / name
        out.mkdir()
        files = {
            'kernel.elf': (ROOT / 'samo-lib/grifo/grifo.elf').read_bytes(),
            'init.app': (ROOT / 'samo-lib/grifo/applications/init/init.app').read_bytes(),
            'bench.app': app.read_bytes(), 'bench.ico': icon.read_bytes(),
            'init.ini': f'bench.ico : bench.app {arguments}\n'.encode(),
            **(extra or {}),
        }
        self.hashes[name] = {k: hashlib.sha256(v).hexdigest() for k, v in files.items()}
        card, flash = out / 'card.img', out / 'flash.rom'
        self.fat.make_image(card, files, 64)
        subprocess.run([sys.executable, str(ROOT / 'samo-lib/mbr/make-flash.py'),
                        str(flash)], check=True, stdout=subprocess.DEVNULL, env=self.env)
        self.hashes[name]['flash.rom'] = sha(flash)
        command = [str(self.args.wremu), '-e', str(flash), '-c', str(card),
                   '-n', '400000000000']
        if uart:
            (out / 'input.txt').write_text(uart)
            command += ['--uart-input', str(out / 'input.txt'),
                        '--uart-start', '250000000', '--uart-gap', '200000']
        (out / 'command.json').write_text(json.dumps(command, indent=2) + '\n')
        with (out / 'boot.log').open('w') as log:
            subprocess.run(command, cwd=out, env=self.env, stdout=log,
                           stderr=subprocess.STDOUT, timeout=self.args.timeout, check=True)
        console = (out / 'boot.log').read_text(errors='replace')
        if 'unknown parameter' in console:
            raise ValueError('unrecognized WREMU_MODEL setting')
        if 'stop reason: powered off' not in console:
            raise ValueError(f'{name} did not power off; inspect {out}/boot.log')
        data = self.fat.read_file(card, report)
        if not data:
            raise ValueError(f'{name} did not save {report}')
        text = data.decode(errors='strict')
        (out / report).write_text(text)
        if name.startswith('riscv-'):
            reference = ROOT / 'riscv/build/ref.txt'
            want = reference.read_text().split()
            got = [word for match in re.findall(
                r'^(\w+) (0x[0-9a-f]{8})\r?$', console, re.M) for word in match]
            if got != want:
                raise ValueError(f'{name}: RISC-V workload checksum mismatch')
            self.hashes[name]['reference'] = sha(reference)
        # Successful cards can be regenerated from the recorded inputs;
        # retain failed cards for diagnosis and successful reports for scoring.
        if not keep_card:
            card.unlink()
        return text


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--set', default='', metavar='NAME=VALUE,...')
    parser.add_argument('--suites', nargs='+', choices=SUITES, default=SUITES)
    parser.add_argument('--wremu', type=Path, default=ROOT / 'emulator/wremu')
    parser.add_argument('--out', type=Path)
    parser.add_argument('--timeout', type=int, default=1800, help='seconds per emulator run')
    parser.add_argument('--compare', type=Path, help='prior scores.json; fail if any score regresses')
    args = parser.parse_args()
    args.wremu = args.wremu.resolve()
    if args.out:
        args.out = args.out.resolve()
        args.out.mkdir(parents=True, exist_ok=False)
    else:
        args.out = Path(tempfile.mkdtemp(prefix='wremu-score.'))
    setting = ','.join(filter(None, [os.environ.get('WREMU_MODEL', ''), args.set]))
    env = {**os.environ, 'WREMU_MODEL': setting, 'WREMU_BOARD_REV': '7',
           'WREMU_UART_TRACE': '', 'WR_FAT_TIME': '1793836800',
           'WR_FAT_FRAGMENT': '0'}
    runner = Runner(args, env)
    result = {'setting': setting, 'emulator_sha256': sha(args.wremu), 'scores': {}, 'missing': {},
              'complete': False,
              'environment': {k: v for k, v in env.items() if k.startswith(('WREMU_', 'WR_FAT_'))},
              'device_sha256': {}, 'inputs': runner.hashes}
    print(f'Artifacts: {args.out}', flush=True)
    start = time.monotonic()
    for suite in args.suites:
        if suite in ('spi', 'mem'):
            bench = 'spibench' if suite == 'spi' else 'membench'
            directory = ROOT / 'build/wr128' / ('spi-dma' if suite == 'spi' else 'mem-dma')
            device = ROOT / 'emulator/tools' / ('spi_dma_bench' if suite == 'spi' else 'mem_dma_bench') / (
                'device-2026-11-05.txt' if suite == 'spi' else 'device-2026-10-02.txt')
            text = runner.run(suite, directory / f'{bench}.app', directory / f'{bench}.ico',
                              'off', f'{bench}.log')
            scores = (score_spi if suite == 'spi' else score_mem)(text, device.read_text())
        elif suite == 'ubench':
            device = UBTOOLS / 'ubench-device.txt'
            text = runner.run(suite, ROOT / 'nuttx/nuttx.app', ROOT / 'nuttx/nuttx.ico',
                              '', 'ubench.txt', uart='ubench; poweroff\n')
            scores = score_ubench(text, device.read_text())
        elif suite == 'pipeline':
            device = ROOT / 'emulator/tools/pipeline_bench/device-2026-11-05.txt'
            directory = ROOT / 'build/wr128/pipeline'
            metadata = device.with_suffix('.json')
            recorded = json.loads(metadata.read_text())
            for name, path in [('pipeline.app', directory / 'pipeline.app'),
                               ('kernel.elf', ROOT / 'samo-lib/grifo/grifo.elf'),
                               ('init.app', ROOT / 'samo-lib/grifo/applications/init/init.app')]:
                if recorded['files'][name] != sha(path):
                    raise ValueError(f'Pipeline hardware used different {name}')
            if sha(device) != recorded['device_log_sha256']:
                raise ValueError('Pipeline device report changed')
            result['device_sha256'][str(metadata.relative_to(ROOT))] = sha(metadata)
            text = runner.run(suite, directory / 'pipeline.app', directory / 'pipeline.ico',
                              'off', 'pipeline.log')
            scores = score_pipeline(text, device.read_text())
        else:
            device = RISC_DEVICE
            if not device.exists():
                message = ('fresh device reports required; the four original RISC-V reports '
                           'include console time and predate the current division code')
                result['missing'][suite] = message
                print(f'{suite:7} UNQUALIFIED: {message}', flush=True)
                continue
            stage = ROOT / 'build/wr128/rv-calibration'
            recorded = json.loads(device.read_text())
            built = json.loads((stage / 'manifest.json').read_text())
            if recorded['manifest'] != built:
                raise ValueError('RISC-V hardware data describes different built inputs')
            for name, path in [('kernel.elf', ROOT / 'samo-lib/grifo/grifo.elf'),
                               ('init.app', ROOT / 'samo-lib/grifo/applications/init/init.app')]:
                if recorded['boot_sha256'][name] != sha(path):
                    raise ValueError(f'RISC-V hardware used different {name}')
            subprocess.run([sys.executable, str(Path(__file__).parent / 'riscv_bench/run.py'),
                            '--wremu', str(args.wremu), '--out', str(args.out / 'riscv'),
                            '--timeout', str(args.timeout)], check=True, env=env)
            run = json.loads((args.out / 'riscv/results.json').read_text())
            runner.hashes.update(run['inputs'])
            model, devices = {}, {}
            for variant, row in built['variants'].items():
                model[variant] = (args.out / 'riscv' / run['reports'][variant]).read_text()
                report = device.parent / recorded['reports'][variant]['file']
                if sha(report) != recorded['reports'][variant]['sha256']:
                    raise ValueError(f'RISC-V device report changed: {variant}')
                devices[variant] = report.read_text()
                result['device_sha256'][str(report.relative_to(ROOT))] = sha(report)
            scores = score_riscv(model, devices)
        result['device_sha256'][str(device.relative_to(ROOT))] = sha(device)
        result['scores'][suite] = scores
        result['elapsed_seconds'] = time.monotonic() - start
        (args.out / 'scores.json').write_text(json.dumps(result, indent=2) + '\n')
        print(f'{suite:7} ' + ' '.join(f'{k}={v:.5f}' if isinstance(v, float) else f'{k}={v}'
                                    for k, v in scores.items()), flush=True)
    result['complete'] = not result['missing']
    result['elapsed_seconds'] = time.monotonic() - start
    (args.out / 'scores.json').write_text(json.dumps(result, indent=2) + '\n')
    if result['missing']:
        raise SystemExit('Calibration incomplete: collect the matching RISC-V hardware run; '
                         'available suite scores are saved in scores.json')
    if args.compare:
        baseline = json.loads(args.compare.read_text())
        for name, digest in result['device_sha256'].items():
            if baseline['device_sha256'].get(name) != digest:
                raise ValueError(f'baseline uses different device data: {name}')
        for name, hashes in result['inputs'].items():
            if baseline['inputs'].get(name) != hashes:
                raise ValueError(f'baseline uses different guest inputs: {name}')
        regressions = [f'{suite}.{key}: {old[key]:.5f} -> {value:.5f}'
                       for suite, scores in result['scores'].items()
                       for old in [baseline['scores'][suite]]
                       for key, value in scores.items()
                       if key != 'cases' and value > old[key] + 1e-9]
        if regressions:
            raise ValueError('timing regressions:\n' + '\n'.join(regressions))


if __name__ == '__main__':
    main()
