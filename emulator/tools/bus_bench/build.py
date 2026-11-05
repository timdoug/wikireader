#!/usr/bin/env python3
"""Build matched dependency controls without changing the frozen pipeline app."""
import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'build/wr128/buspipe'
SOURCE = ROOT / 'emulator/tools/pipeline_bench/pipeline.c'
EXTRA = [
    ('h_indep0', 'ld.h %r10,[%r7]\nxor %r11,%r12', 5),
    ('h_indep1', 'ld.h %r10,[%r7]\nld.w %r12,%r12\nxor %r11,%r12', 5),
    ('h_indep2', 'ld.h %r10,[%r7]\nld.w %r12,%r12\nld.w %r12,%r12\nxor %r11,%r12', 5),
    ('w_indep0', 'ld.w %r10,[%r7]\nxor %r11,%r12', 5),
    ('w_indep1', 'ld.w %r10,[%r7]\nld.w %r12,%r12\nxor %r11,%r12', 5),
    ('w_indep2', 'ld.w %r10,[%r7]\nld.w %r12,%r12\nld.w %r12,%r12\nxor %r11,%r12', 5),
    ('word_use2', 'ld.w %r10,[%r7]\nld.w %r12,%r12\nld.w %r12,%r12\nxor %r11,%r10', 5),
    ('h_cmp_dep', 'ld.h %r10,[%r7]\ncmp %r11,%r10', 5),
    ('h_cmp_indep', 'ld.h %r10,[%r7]\ncmp %r11,%r12', 5),
    ('w_cmp_dep', 'ld.w %r10,[%r7]\ncmp %r11,%r10', 5),
    ('w_cmp_indep', 'ld.w %r10,[%r7]\ncmp %r11,%r12', 5),
    ('h_mov_dep', 'ld.h %r10,[%r7]\nld.w %r11,%r10', 5),
    ('h_mov_indep', 'ld.h %r10,[%r7]\nld.w %r11,%r12', 5),
    ('w_mov_dep', 'ld.w %r10,[%r7]\nld.w %r11,%r10', 5),
    ('w_mov_indep', 'ld.w %r10,[%r7]\nld.w %r11,%r12', 5),
    ('h_overwrite', 'ld.h %r10,[%r7]\nld.w %r10,5', 5),
    ('w_overwrite', 'ld.w %r10,[%r7]\nld.w %r10,5', 5),
    ('h_waw', 'ld.h %r10,[%r7]\nld.w %r10,%r12', 16),
    ('w_waw', 'ld.w %r10,[%r7]\nld.w %r10,%r12', 16),
    ('h_ext_indep', 'ld.h %r10,[%r7]\next 7\nand %r11,-2', 5),
    ('h_two_loads', 'ld.h %r10,[%r7]\nld.h %r11,[%r7]', 5),
    ('w_two_loads', 'ld.w %r10,[%r7]\nld.w %r11,[%r7]', 5),
    ('ext1_imm', 'ext 0\nld.w %r10,5', 5),
    ('ext2_imm', 'ext 0\next 0\nld.w %r10,5', 5),
]


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    source = SOURCE.read_text()
    base = dict((k, int(v)) for k, v in re.findall(r'ROW\((\w+),\s*(\d+)\)', source))
    assert len(base) == 20 and source.count('struct probe {') == 1
    definitions = '\n'.join(f'PROBE({name}, {json.dumps(body)});' for name, body, _ in EXTRA)
    source = source.replace('struct probe {', definitions + '\n\nstruct probe {')
    assert source.count('ROW(mixed, 4),') == 1
    source = source.replace('ROW(mixed, 4),', 'ROW(mixed, 4),\n    ' +
                            ', '.join(f'ROW({name}, {value})' for name, _, value in EXTRA) + ',')
    source = source.replace('pipeline.log', 'buspipe.log').replace('PIPELINE v1', 'BUSPIPE v1')
    source = source.replace('END PIPELINE', 'END BUSPIPE').replace('Pipeline timing probe', 'Bus dependency probe')
    source = source.replace('placement < 8', 'placement < 2')
    generated = OUT / 'buspipe.c'
    generated.write_text(source)
    subprocess.run(['make', '-s', '-C', str(SOURCE.parent), f'OUT={OUT}',
                    'PROGRAM=buspipe', f'SOURCE={generated}'], check=True)
    probes = {**base, **{name: value for name, _, value in EXTRA}}
    manifest = {'version': 1, 'probes': probes, 'placements': [('sdram', 0), ('a0', 0), ('a0', 2)],
                'data': ['a0', 'sdram'], 'cases': len(probes) * 6,
                'source_sha256': hashlib.sha256(generated.read_bytes()).hexdigest(),
                'app_sha256': hashlib.sha256((OUT / 'buspipe.app').read_bytes()).hexdigest()}
    (OUT / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'{manifest["cases"]} matched bus cases: {OUT}/buspipe.app')


if __name__ == '__main__':
    main()
