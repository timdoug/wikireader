#!/usr/bin/env python3
"""Measure cold boot to the first displayed Doom frame through factory-style FLASH boot."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('wad', type=Path)
    parser.add_argument('--app', type=Path, default=ROOT / 'doom/doom.app')
    parser.add_argument('--map', type=Path, default=ROOT / 'doom/doom.map')
    parser.add_argument('--args', default='', help='Doom arguments; default is the title screen')
    args = parser.parse_args()
    work = ROOT / 'build/doom'
    work.mkdir(parents=True, exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix='boot-', dir=work))
    shutil.copyfile(args.app, out / 'doom.app')
    shutil.copyfile(args.map, out / 'doom.map')
    symbols = dict((name, address) for address, name in re.findall(
        r'^\s+(0x[0-9a-f]+)\s+(\w+)\s*$', (out / 'doom.map').read_text(), re.M))
    subprocess.run([sys.executable, str(ROOT / 'doom/make-flash.py'), str(out / 'flash.rom')], check=True)
    subprocess.run([sys.executable, str(ROOT / 'doom/make-card.py'), str(out / 'card.img'),
                    str(args.wad.resolve()), '--app', str(out / 'doom.app'), '--args', args.args], check=True)
    start, end = symbols['wr_engine_init'], symbols['doom_frame_ready']
    cmd = [str(ROOT / 'emulator/wremu'), '-R', '-e', str(out / 'flash.rom'),
           '-c', str(out / 'card.img'), '-n', '500000000',
           '-M', start, '-b', end, '-Y', start + ',' + end, '-F', 'profile.txt']
    probes = ['wr_engine_init', 'W_InitMultipleFiles', 'R_InitTextures',
              'R_InitSpriteLumps', 'R_InitColormaps', 'HU_Init', 'wr_video_init', 'doom_frame_ready']
    for name in probes:
        cmd += ['-X', symbols[name] + ',' + name]
    with (out / 'run.log').open('w') as log:
        subprocess.run(cmd, cwd=out, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=300)
    log = (out / 'run.log').read_text(errors='replace')
    if ('Doom ready:' not in log or any(s in log for s in ('Panic:', 'Error:', 'misaligned'))
            or not re.search(r'--- wdt: \d+ kicks, 0 timeouts', log)):
        raise RuntimeError(f'Firmware failed; see {out / "run.log"}')
    stages = {}
    for name in probes:
        match = re.search(r'--- probe ' + name + r'\s+1 hits\s+first\s+\d+/\s*([\d.]+)ms', log)
        if not match:
            raise RuntimeError(f'Missing startup stage {name}; see {out / "run.log"}')
        stages[name] = float(match[1])
    report = {
        'arguments': args.args, 'cold_boot_ms': stages['doom_frame_ready'],
        'engine_startup_ms': round(stages['doom_frame_ready'] - stages['wr_engine_init'], 1),
        'stages_ms': stages,
        'app_sha256': hashlib.sha256((out / 'doom.app').read_bytes()).hexdigest(),
        'wad_sha256': hashlib.sha256(args.wad.read_bytes()).hexdigest(),
        'hardware_tested': False,
    }
    (out / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'{report["cold_boot_ms"] / 1000:.2f} seconds to first frame; results: {out}', flush=True)


if __name__ == '__main__':
    main()
