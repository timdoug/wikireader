#!/usr/bin/env python3
"""Verify repeated watchdog resets through FLASH, Grifo and the real launcher."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify_boot(text, returncode, executable_unchanged):
    # The instruction limit can interrupt an unfinished UART progress line.
    summaries = re.findall(r'--- resets: (\d+);', text)
    resets = int(summaries[0]) if len(summaries) == 1 else 0
    boots = text.count('WDT_RESET_APP')
    chains = re.findall(r'load: kernel\.elf.*?Grifo starting.*?init starting.*?WDT_RESET_APP', text, re.S)
    checks = {'exit status': returncode == 0,
              'watchdog reset count': resets >= 3 and text.count('[watchdog reset]') == resets,
              'complete repeated boot chains': len(chains) == boots and boots >= 4,
              'reset re-enters FLASH rather than the app': boots in (resets, resets + 1),
              'bounded run across resets': 'instruction limit reached' in text,
              'no guest faults': not any(s in text for s in ('C33 exception', 'Panic:', 'unknown parameter')),
              'unchanged executable': executable_unchanged}
    return resets, boots, checks


def run(args, stage):
    cc = args.toolchain / 'c33-epson-elf-gcc'
    app = stage / 'wdtreset.app'
    command = [str(cc), '-O2', '-mc33pe', '-mlong-calls', '-std=gnu99',
               '-fgnu89-inline', '-fno-builtin', '-nostdlib',
               '-I' + str(ROOT / 'samo-lib/grifo/include'),
               '-I' + str(ROOT / 'samo-lib/include'),
               '-I' + str(ROOT / 'samo-lib/mini-libc/include'),
               '-Wl,-T,' + str(ROOT / 'samo-lib/grifo/lds/application.lds'),
               str(ROOT / 'emulator/tools/test_wdt_reset.c'),
               str(ROOT / 'samo-lib/grifo/lib/libgrifo.a'),
               str(ROOT / 'samo-lib/mini-libc/lib/libc.a'), '-lgcc', '-o', str(app)]
    subprocess.run(command, check=True)
    spec = importlib.util.spec_from_file_location('fat', ROOT / 'emulator/tools/mem_dma_bench/run.py')
    fat = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fat)
    files = {'kernel.elf': (ROOT / 'samo-lib/grifo/grifo.elf').read_bytes(),
             'init.app': (ROOT / 'samo-lib/grifo/applications/init/init.app').read_bytes(),
             'wdtreset.app': app.read_bytes(), 'wdtreset.ico': bytes(512),
             'init.ini': b'wdtreset.ico : wdtreset.app\n'}
    card = stage / 'card.img'
    fat.make_image(card, files, 64)
    flash = fat.build_flash(stage)
    command = [str(args.wremu), '-R', '-e', str(flash), '-c', str(card), '-n', str(args.limit)]
    identity = {'emulator_sha256': sha(args.wremu), 'flash_sha256': sha(flash),
                'inputs': {name: hashlib.sha256(data).hexdigest() for name, data in files.items()},
                'setting': os.environ.get('WREMU_MODEL', ''), 'command': command}
    (stage / 'inputs.json').write_text(json.dumps(identity, indent=2) + '\n')
    with (stage / 'boot.log').open('w') as log:
        result = subprocess.run(command, cwd=stage, stdout=log, stderr=subprocess.STDOUT, timeout=120)
    text = (stage / 'boot.log').read_text(errors='replace')
    resets, boots, checks = verify_boot(text, result.returncode,
                                      sha(args.wremu) == identity['emulator_sha256'])
    report = {**identity, 'resets': resets, 'app_boots': boots, 'checks': checks,
              'returncode': result.returncode,
              'complete': all(checks.values()), 'log_sha256': sha(stage / 'boot.log')}
    (stage / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    if not report['complete']:
        raise SystemExit(f'Watchdog reboot failed: {[k for k, v in checks.items() if not v]}; {stage / "boot.log"}')
    print(f'Watchdog reboot: {resets} resets, {boots} complete FLASH/Grifo/init/app boots; run stayed bounded')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--wremu', type=Path, default=ROOT / 'emulator/wremu')
    parser.add_argument('--toolchain', type=Path, default=ROOT / 'host-tools/toolchain-c33/work/install/bin')
    parser.add_argument('--out', type=Path)
    parser.add_argument('--limit', type=int, default=100000000)
    args = parser.parse_args()
    args.wremu = args.wremu.resolve()
    args.toolchain = args.toolchain.resolve()
    if args.out:
        stage = args.out.resolve()
        stage.mkdir(parents=True, exist_ok=False)
        run(args, stage)
    else:
        with tempfile.TemporaryDirectory(prefix='wr-wdt-reset-') as tmp:
            run(args, Path(tmp))


if __name__ == '__main__':
    main()
