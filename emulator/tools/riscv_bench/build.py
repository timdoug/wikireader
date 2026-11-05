#!/usr/bin/env python3
"""Build isolated placement variants for one matching hardware calibration run."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'build/wr128/rv-calibration'
VARIANTS = {'sdram': ('rvsdr', 0, 0, 0), 'code': ('rva0', 1, 0, 0),
            'code+state': ('rva0s', 1, 1, 0), 'asm': ('rvasm', 1, 1, 1)}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    manifest = {'variants': {}, 'inputs': {}}
    for variant, (stem, fast, state, assembly) in VARIANTS.items():
        work = OUT / variant
        if work.exists():
            shutil.rmtree(work)
        work.mkdir()
        for source in (ROOT / 'riscv').iterdir():
            if source.is_file() and source.suffix in ('.c', '.h', '.s', '.lds', '.ico'):
                shutil.copy2(source, work / source.name)
                manifest['inputs'][str(source.relative_to(ROOT))] = sha(source)
        shutil.copy2(ROOT / 'riscv/Makefile', work / 'Makefile')
        subprocess.run(['make', '-s', 'riscv.app', f'ROOT_DIR={ROOT}',
                        f'TOOLCHAIN_BIN={ROOT}/host-tools/toolchain-c33/work/install/bin',
                        f'FAST={fast}', f'STATE={state}', f'ASM={assembly}', 'JIT=0'],
                       cwd=work, check=True)
        app = OUT / f'{stem}.app'
        shutil.copy2(work / 'riscv.app', app)
        manifest['variants'][variant] = {'app': app.name, 'report': f'{stem}.txt',
                                         'sha256': sha(app)}
    for source, target in [('riscv/build/rvbench.bin', 'rvbench.bin'),
                           ('riscv/build/ref.txt', 'ref.txt'), ('riscv/riscv.ico', 'rvcal.ico')]:
        shutil.copy2(ROOT / source, OUT / target)
        manifest['inputs'][source] = sha(OUT / target)
    (OUT / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'Built four calibration variants: {OUT}')


if __name__ == '__main__':
    main()
