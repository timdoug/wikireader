#!/usr/bin/env python3
"""Run the same automatic benchmark shipped to hardware, and extract its SD log."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('wad', type=Path)
    args = parser.parse_args()
    out = Path(tempfile.mkdtemp(prefix='trace-', dir=ROOT / 'build/doom'))
    for name in ('doom.app', 'doom.map', 'doom.dump'):
        shutil.copyfile(ROOT / 'doom' / name, out / name)
    subprocess.run([sys.executable, str(ROOT / 'doom/make-flash.py'), str(out / 'flash.rom')], check=True)
    subprocess.run([sys.executable, str(ROOT / 'doom/make-card.py'), str(out / 'card.img'),
                    str(args.wad.resolve()), '--app', str(out / 'doom.app'), '--args=-wrbench'], check=True)
    symbols = dict((name, address) for address, name in re.findall(
        r'^\s+(0x[0-9a-f]+)\s+(\w+)\s*$', (out / 'doom.map').read_text(), re.M))
    command = [str(ROOT / 'emulator/wremu'), '-e', str(out / 'flash.rom'),
               '-c', str(out / 'card.img'), '-n', '1200000000',
               '-Y', symbols['doom_benchmark_start'] + ',' + symbols['doom_benchmark_end'],
               '-F', 'profile.txt', '-X', symbols['doom_frame_ready'] + ',doom_frame_ready']
    with (out / 'run.log').open('w') as log:
        subprocess.run(command, cwd=out, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=360)
    serial = (out / 'run.log').read_text(errors='replace')
    if ('Doom benchmark complete' not in serial
            or any(s in serial for s in ('Panic:', 'Error:', 'misaligned', 'log failed'))
            or not re.search(r'--- wdt: \d+ kicks, 0 timeouts', serial)):
        raise RuntimeError(f'Target execution failed: {out}')
    # This helper reads the known 512-byte-cluster fixture, never a device.
    reader = module('mem_dma_fixture', ROOT / 'emulator/tools/mem_dma_bench/run.py')
    raw = reader.read_file(out / 'card.img', 'doomperf.log')
    if not raw:
        raise RuntimeError(f'No persisted performance log: {out}')
    (out / 'doomperf.log').write_bytes(raw)
    report = module('trace_report', ROOT / 'doom/trace-report.py')
    runs = report.summarize(out / 'doomperf.log')
    benchmarks = [w for r in runs for w in r['windows'] if w.get('comparable')]
    if len(benchmarks) != 1:
        raise RuntimeError(f'Incomplete or contaminated benchmark: {out}')
    if len(runs) != 1 or [w['kind'] for w in runs[0]['windows']] != ['WARMUP', 'BENCH']:
        raise RuntimeError(f'Default benchmark unexpectedly continued tracing: {out}')
    hashes = {name: hashlib.sha256(path.read_bytes()).hexdigest() for name, path in {
        'app': out / 'doom.app', 'wad': args.wad, 'kernel': ROOT / 'samo-lib/grifo/grifo.elf',
        'init': ROOT / 'samo-lib/grifo/applications/init/init.app', 'emulator': ROOT / 'emulator/wremu'
    }.items()}
    rows = [line.split() for line in (out / 'profile.txt').read_text().splitlines()]
    frames = sum(int(r[1]) for r in rows if int(r[0], 16) == int(symbols['doom_frame_ready'], 16))
    window = re.search(r'--- window .*?: \d+ instructions, ([\d.]+) ms', serial)
    if not window or frames != benchmarks[0]['frames']:
        raise RuntimeError(f'External frame probe disagrees with the persisted log: {out}')
    probe_ms = float(window[1])
    if abs(probe_ms - benchmarks[0]['elapsed_us'] / 1000) > 1:
        raise RuntimeError(f'External timer disagrees with the persisted log: {out}')
    result = dict(hardware_tested=False, sha256=hashes, runs=runs,
                  external_probe_elapsed_ms=probe_ms, external_probe_frames=frames,
                  external_probe_fps=frames * 1000 / probe_ms,
                  command=command)
    (out / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(f'{benchmarks[0]["fps"]:.2f} modeled fps, persisted on card: {out}', flush=True)


if __name__ == '__main__':
    main()
