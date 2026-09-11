#!/usr/bin/env python3
"""Compare 1,500 demo frames against a saved engine.c + vendor/PureDOOM.h."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('wad', type=Path)
    parser.add_argument('--reference', type=Path, required=True)
    args = parser.parse_args()
    out = Path(tempfile.mkdtemp(prefix='replay-', dir=ROOT / 'build/doom'))
    outputs = []
    for name, engine in [('reference', args.reference.resolve() / 'engine.c'),
                         ('current', ROOT / 'doom/engine.c')]:
        directory = out / name
        directory.mkdir()
        shutil.copyfile(args.wad, directory / 'doom1.wad')
        cmd = ['cc', '-std=gnu11', '-w', '-O1', '-fwrapv', '-fno-strict-aliasing',
               '-fsanitize=address', '-I', str(ROOT / 'doom'),
               '-DWR_REPLAY_ENGINE=' + json.dumps(str(engine)),
               str(ROOT / 'doom/tests/replay.c'), str(ROOT / 'doom/c33_math.c'),
               '-o', str(directory / 'replay')]
        subprocess.run(cmd, check=True)
        with (directory / 'frames.txt').open('w') as frames, (directory / 'run.log').open('w') as log:
            subprocess.run([str(directory / 'replay')], cwd=directory, check=True,
                           stdout=frames, stderr=log, timeout=120)
        outputs.append((directory / 'frames.txt').read_bytes())
    if outputs[0] != outputs[1] or len(outputs[1].splitlines()) != 1500:
        raise RuntimeError(f'Demo replay differs: {out}')
    report = {'frames': 1500, 'identical': True, 'address_sanitizer': 'passed',
              'frames_sha256': hashlib.sha256(outputs[1]).hexdigest(),
              'wad_sha256': hashlib.sha256(args.wad.read_bytes()).hexdigest()}
    (out / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'1,500 demo frames, palettes and player states match: {out}')


if __name__ == '__main__':
    main()
