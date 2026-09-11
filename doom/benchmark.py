#!/usr/bin/env python3
"""Measure a stationary opening scene or recorded movement/combat."""
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
    parser.add_argument('--scene', choices=['spawn', 'demo'], default='spawn')
    args = parser.parse_args()
    work = ROOT / 'build/doom'
    work.mkdir(parents=True, exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix='benchmark-', dir=work))
    shutil.copyfile(args.app, out / 'doom.app')
    shutil.copyfile(args.map, out / 'doom.map')
    app_hash = hashlib.sha256((out / 'doom.app').read_bytes()).hexdigest()
    symbols = dict((name, address) for address, name in re.findall(
        r'^\s+(0x[0-9a-f]+)\s+(\w+)\s*$', (out / 'doom.map').read_text(), re.M))
    frame = symbols['doom_frame_ready']
    subprocess.run([sys.executable, str(ROOT / 'doom/make-flash.py'), str(out / 'flash.rom')], check=True)
    subprocess.run([sys.executable, str(ROOT / 'doom/make-card.py'), str(out / 'card.img'),
                    str(args.wad.resolve()), '--app', str(out / 'doom.app'),
                    '--args', '-playdemo demo1' if args.scene == 'demo' else
                    '-warp 1 1 -skill 2'], check=True)
    cmd = [str(ROOT / 'emulator/wremu'), '-R', '-e', str(out / 'flash.rom'),
           '-c', str(out / 'card.img'), '-n', '1200000000',
           '-y', '30000,40000', '-F', 'profile.txt', '-X', frame + ',doom_frame_ready']
    with (out / 'run.log').open('w') as log:
        subprocess.run(cmd, cwd=out, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=360)
    log = (out / 'run.log').read_text(errors='replace')
    if ('Doom ready:' not in log or any(s in log for s in ('Panic:', 'Error:', 'misaligned'))
            or not re.search(r'--- wdt: \d+ kicks, 0 timeouts', log)):
        raise RuntimeError(f'Firmware failed; see {out / "run.log"}')
    window = re.search(r'--- window 30000\.\.40000 ms: \d+ instructions, ([\d.]+) ms', log)
    if not window or abs(float(window[1]) - 10000) > 0.1:
        raise RuntimeError('Run did not complete the measurement window')
    rows = [line.split() for line in (out / 'profile.txt').read_text().splitlines()]
    frames = sum(int(row[1]) for row in rows if int(row[0], 16) == int(frame, 16))
    if not frames:
        raise RuntimeError('No displayed frames in the measurement window')
    def hits(name):
        return sum(int(row[1]) for row in rows if int(row[0], 16) == int(symbols[name], 16))
    world_frames = hits('R_RenderPlayerView')
    title_frames, wipe_steps = hits('D_PageDrawer'), hits('D_UpdateWipe')
    # The retained screenshot is taken after the run, which may be later
    # than the measurement window. Validate that the window itself is play.
    if title_frames or wipe_steps or abs(world_frames - frames) > 2:
        raise RuntimeError('Measurement window contains a title/transition or missing world renders')
    report = {
        'scene': ('shareware built-in DEMO1, recorded movement/combat' if args.scene == 'demo'
                  else 'shareware E1M1, skill 2, standing at spawn, no input'),
        'window_ms': [30000, 40000], 'frames': frames, 'fps': frames / 10,
        'app_sha256': app_hash,
        'wad_sha256': hashlib.sha256(args.wad.read_bytes()).hexdigest(),
        'hardware_tested': False,
        'shots_in_window': hits('P_FireWeapon'),
        'world_frames_in_window': world_frames,
        'title_frames_in_window': title_frames,
        'wipe_steps_in_window': wipe_steps,
    }
    if args.scene == 'demo' and not report['shots_in_window']:
        raise RuntimeError('Demo benchmark did not exercise combat in the window')
    (out / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'{frames / 10:.1f} modeled fps; results: {out}', flush=True)


if __name__ == '__main__':
    main()
