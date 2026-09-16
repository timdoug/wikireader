#!/usr/bin/env python3
"""Boot the actual C33 app and exercise gameplay and save/load through Grifo."""
import argparse
import concurrent.futures
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('wad', type=Path)
    parser.add_argument('--scenario', choices=['gameplay', 'save-load', 'title', 'all'], default='all')
    args = parser.parse_args()
    work = ROOT / 'build/doom'
    work.mkdir(parents=True, exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix='smoke-', dir=work))
    symbols = dict((name, address) for address, name in re.findall(
        r'^\s+(0x[0-9a-f]+)\s+(\w+)\s*$', (ROOT / 'doom/doom.map').read_text(), re.M))
    subprocess.run([sys.executable, str(ROOT / 'samo-lib/mbr/make-flash.py'), str(out / 'flash.rom')], check=True)
    scenarios = {
        'gameplay': ['-N', '0,72000000', '-T', '120,20,90000000', '-T', '220,20,110000000',
                     '-N', '1,130000000', '-T', '60,195,150000000', '-T', '60,195,175000000'],
        # Leave time for the save/card write and menu debounce before the
        # next action; tightly packed taps depend on exact code alignment.
        'save-load': ['-N', '2,72000000', '-N', '1,96000000', '-N', '1,120000000', '-N', '1,144000000',
                      '-N', '0,168000000', '-N', '0,192000000', '-N', '0,216000000', '-N', '2,288000000',
                      '-T', '120,20,324000000', '-N', '0,360000000', '-N', '0,396000000'],
        # Enter the menu before the attract-mode demo starts its own wipe.
        'title': ['-N', '0,12000000', '-N', '0,24000000', '-N', '0,36000000', '-N', '0,48000000'],
    }

    def run(name):
        directory = out / name
        directory.mkdir()
        subprocess.run([sys.executable, str(ROOT / 'doom/make-card.py'), str(directory / 'card.img'),
                        str(args.wad.resolve()), '--args', '' if name == 'title' else '-warp 1 1 -skill 2'], check=True)
        # Menu functions are reached through pointers. The compiler inlines
        # G_DeferedInitNew, so its standalone symbol is not a useful probe.
        probes = ['doom_frame_ready', 'P_FireWeapon', 'G_DoSaveGame', 'G_DoLoadGame', 'G_InitNew', 'M_ChooseSkill']
        cmd = [str(ROOT / 'emulator/wremu'), '-e', str(out / 'flash.rom'), '-c', str(directory / 'card.img'),
               '-n', '650000000', '-Z', symbols['doom_frame_ready'], *scenarios[name]]
        for probe in probes:
            cmd += ['-X', symbols[probe] + ',' + probe]
        print(f'{name}: running C33 firmware', flush=True)
        with (directory / 'run.log').open('w') as log:
            subprocess.run(cmd, cwd=directory, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=300)
        log = (directory / 'run.log').read_text(errors='replace')
        counts = {p: int(re.search(r'--- probe ' + p + r'\s+(\d+) hits', log)[1]) for p in probes}
        if ('Doom ready:' not in log or any(s in log for s in ('Panic:', 'misaligned', 'Error:', 'Doom exited: -'))
                or counts['doom_frame_ready'] < 20):
            raise RuntimeError(f'{name}: boot/render failed; see {directory / "run.log"}')
        required = {'gameplay': ['P_FireWeapon'], 'save-load': ['G_DoSaveGame', 'G_DoLoadGame'], 'title': ['M_ChooseSkill', 'G_InitNew']}
        first_frame = int(re.search(r'--- probe doom_frame_ready\s+\d+ hits\s+first\s+(\d+)/', log)[1])
        for probe in required[name]:
            last_hit = int(re.search(r'--- probe ' + probe + r'.*last\s+(\d+)/', log)[1])
            # FLASH startup also executes in SDRAM: reject coincidental
            # address hits before the app has drawn its first frame.
            if not counts[probe] or last_hit < first_frame:
                raise RuntimeError(f'{name}: no {probe} call; see {directory / "run.log"}')
        if not re.search(r'--- wdt: \d+ kicks, 0 timeouts', log):
            raise RuntimeError(f'{name}: watchdog timeout')
        print(f'{name}: passed {counts}', flush=True)
        return counts

    names = list(scenarios) if args.scenario == 'all' else [args.scenario]
    results, failures = {}, {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
        futures = {pool.submit(run, name): name for name in names}
        for future in concurrent.futures.as_completed(futures):
            name = futures[future]
            try:
                results[name] = future.result()
            except Exception as error:
                failures[name] = str(error)
                print(f'{name}: FAILED: {error}', flush=True)
    report = {'app_sha256': hashlib.sha256((ROOT / 'doom/doom.app').read_bytes()).hexdigest(),
              'wad_sha256': hashlib.sha256(args.wad.read_bytes()).hexdigest(), 'scenarios': results,
              'failures': failures, 'hardware_tested': False}
    (out / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'Results: {out}', flush=True)
    if failures:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
