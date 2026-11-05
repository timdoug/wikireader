#!/usr/bin/env python3
"""Run the reader measurements through FLASH, Grifo and the real launcher."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from score_model import ROOT, fat_helper
from report import summarize, startup as parse_startup


def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def finish(out, inputs):
    manifest = inputs['manifest']
    for name in ('wremu', 'flash.rom'):
        if sha(out / name) != inputs['execution'][name]:
            raise ValueError(f'Workflow executable changed: {name}')
    for name, expected in inputs['files'].items():
        data = fat_helper().read_file(out / 'card.img', name)
        if data is None or hashlib.sha256(data).hexdigest() != expected:
            raise ValueError(f'Executed workflow input differs: {name}')
    text = fat_helper().read_file(out / 'card.img', 'zimbench.txt')
    if not text:
        raise ValueError('Reader saved no workflow report')
    text = text.decode()
    rows = summarize(text, manifest)
    boot = (out / 'boot.log').read_text(errors='replace')
    if any(marker in boot for marker in ('Panic:', 'C33 exception', 'watchdog reset', 'unknown parameter')):
        raise ValueError('Workflow boot encountered an emulator or firmware fault')
    for marker in ('load: kernel.elf', 'Grifo starting', 'init starting',
                   'starting ZIM reader', 'stop reason: powered off'):
        if marker not in boot:
            raise ValueError(f'Workflow did not finish through the boot chain: {marker}')
    (out / 'zimbench.txt').write_text(text)
    startup = fat_helper().read_file(out / 'card.img', 'zimboot.log')
    if not startup or b'END ZIMBOOT' not in startup:
        raise ValueError('Missing startup phase report')
    startup_phases = parse_startup(startup.decode())
    (out / 'zimboot.log').write_bytes(startup)
    result = {'complete': True, 'inputs': inputs, 'emulator_sha256': sha(out / 'wremu'),
              'flash_sha256': sha(out / 'flash.rom'), 'report_sha256': sha(out / 'zimbench.txt'),
              'startup_sha256': sha(out / 'zimboot.log'), 'phases': rows,
              'startup': startup_phases,
              'hardware_compared': False,
              'scope': 'lookup, first-screen rendering and four fixed viewport steps; not a full input/idle workflow'}
    (out / 'results.json').write_text(json.dumps(result, indent=2) + '\n')
    print(f'{len(rows)} repeated workflow phases verified: {out}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fixture', type=Path, default=ROOT / 'build/wr128/workflow-calibration/fixture')
    parser.add_argument('--wremu', type=Path, default=ROOT / 'emulator/wremu')
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--collect-only', action='store_true', help='validate a completed manual run')
    parser.add_argument('--timeout', type=int, default=900)
    parser.add_argument('--profile', action='store_true', help='retain opcode and PC timing profiles')
    args = parser.parse_args()
    args.out = args.out.resolve()
    if args.collect_only:
        inputs = json.loads((args.out / 'inputs.json').read_text())
        finish(args.out, inputs)
        return
    inputs = json.loads((args.fixture / 'inputs.json').read_text())
    args.out.mkdir(parents=True, exist_ok=False)
    helper = fat_helper()
    for name, expected in inputs['files'].items():
        data = helper.read_file(args.fixture / 'card.img', name)
        if data is None or hashlib.sha256(data).hexdigest() != expected:
            raise ValueError(f'Workflow fixture input changed: {name}')
    fixture_hash = sha(args.fixture / 'card.img')
    if fixture_hash != inputs['fixture_sha256']:
        raise ValueError('Workflow fixture image changed since preparation')
    subprocess.run(['cp', '-c', str(args.fixture / 'card.img'), str(args.out / 'card.img')], check=True)
    if sha(args.out / 'card.img') != fixture_hash:
        raise ValueError('Workflow card clone differs from its fixture')
    shutil.copy2(args.wremu, args.out / 'wremu')
    subprocess.run([sys.executable, str(ROOT / 'samo-lib/mbr/make-flash.py'),
                    str(args.out / 'flash.rom')], check=True, stdout=subprocess.DEVNULL)
    env = {**os.environ, 'WREMU_BOARD_REV': '7', 'WREMU_UART_TRACE': '',
           'WR_FAT_TIME': '1793836800'}
    settings = {k: v for k, v in env.items() if k.startswith(('WREMU_', 'WR_FAT_'))}
    inputs = {**inputs, 'execution': {'wremu': sha(args.out / 'wremu'),
              'flash.rom': sha(args.out / 'flash.rom'), 'environment': settings}}
    (args.out / 'inputs.json').write_text(json.dumps(inputs, indent=2) + '\n')
    command = [str(args.out / 'wremu'), '-e', str(args.out / 'flash.rom'),
               '-c', str(args.out / 'card.img'), '-n', '400000000000']
    if 'launcher_tap' in inputs:
        command += ['-T', ','.join(map(str, inputs['launcher_tap']))]
    if args.profile:
        command += ['-P', '-H', '-F', str(args.out / 'profile.txt')]
    (args.out / 'command.json').write_text(json.dumps(command, indent=2) + '\n')
    with (args.out / 'boot.log').open('w') as log:
        subprocess.run(command, cwd=args.out, env=env, stdout=log,
                       stderr=subprocess.STDOUT, timeout=args.timeout, check=True)
    finish(args.out, inputs)


if __name__ == '__main__':
    main()
