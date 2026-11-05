#!/usr/bin/env python3
"""Build variable-length local-port loops, preserving the frozen probes."""
import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'build/wr128/portq'
SOURCE = ROOT / 'emulator/tools/pipeline_bench/pipeline.c'
UNROLLS = (1, 2, 3, 4, 8, 12, 16)
SHAPES = (
    ('base', 'ld.w %r10,%r10', 5),
    ('nop', 'nop', 5),
    ('ext1', 'ext 7\nand %r10,-2', 4),
    ('ext2', 'ext 0\next 7\nand %r10,-2', 4),
    ('ldh', 'ld.h %r10,[%r7]', 13),
    ('ldw', 'ld.w %r10,[%r7]', 13),
    ('store', 'ld.w [%r8],%r10', 5),
    ('h_nop', 'ld.h %r10,[%r7]\nnop', 13),
    ('w_nop', 'ld.w %r10,[%r7]\nnop', 13),
    ('h_xor_dep', 'ld.h %r10,[%r7]\nxor %r11,%r10', 13),
    ('h_xor_indep', 'ld.h %r10,[%r7]\nxor %r11,%r12', 13),
    ('w_xor_dep', 'ld.w %r10,[%r7]\nxor %r11,%r10', 13),
    ('w_xor_indep', 'ld.w %r10,[%r7]\nxor %r11,%r12', 13),
    ('h_two', 'ld.h %r10,[%r7]\nld.h %r11,[%r7]', 0),
    ('w_two', 'ld.w %r10,[%r7]\nld.w %r11,[%r7]', 0),
    ('ext1_load', 'ext 0\nld.w %r10,[%r7]', 13),
    ('ext2_load', 'ext 0\next 0\nld.w %r10,[%r7]', 13),
    ('load_ext', 'ld.h %r10,[%r7]\next 7\nand %r10,-2', 12),
    ('move_ext', 'ld.w %r10,%r10\next 7\nand %r10,-2', 4),
    ('mixed', 'ext 0\next 0\nld.w %r10,5\nld.h %r10,[%r7]\n'
              'ext 7\nand %r10,-2\nxor %r11,%r10', 12),
)

MACRO = r'''#define PROBE_N(name, count, body) \
extern probe_fn name; extern char name##_end[]; \
asm(".pushsection .text.probes,\"ax\"\n.balign 16\n.global " #name "\n" #name ":\n" \
    "ld.w %r10,5\nld.w %r11,0\nld.w %r12,16\n.balign 16\n" \
    ".L" #name "_loop:\n.rept " #count "\n" body "\n.endr\n" \
    "sub %r6,1\njrne .L" #name "_loop\nld.w %r4,%r10\nxor %r4,%r11\nret\n" \
    ".global " #name "_end\n" #name "_end:\n.popsection\n")
'''


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f'Frozen probe template changed at: {old[:70]}')
    return text.replace(old, new)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    text = SOURCE.read_text()
    variants = {f'{name}_u{u}': {'shape': name, 'unroll': u, 'result': result,
                                'bytes': 16 + 2 * len(body.splitlines()) * u + 10}
                for name, body, result in SHAPES for u in UNROLLS}
    definitions = MACRO + '\n' + '\n'.join(
        f'PROBE_N({name}_u{u}, {u}, {json.dumps(body)});'
        for name, body, _ in SHAPES for u in UNROLLS)
    table = ('struct probe { const char *name; probe_fn *fn; char *end; unsigned result, unroll; };\n'
             '#define ROW(name, result, u) { #name, name, name##_end, result, u }\n'
             'static const struct probe probes[] = {\n' + ''.join(
                 f'    ROW({name}, {row["result"]}, {row["unroll"]}),\n' for name, row in variants.items()) + '};\n\n')
    begin, end = text.index('#define PROBE('), text.index('/* Application-owned memory')
    text = text[:begin] + definitions + '\n\n' + table + text[end:]
    text = replace_once(text, '#define UNROLL 8\n', '')
    text = replace_once(text, 'unroll=%u repeats=%u', 'unroll=variable repeats=%u')
    text = replace_once(text, 'PASSES, UNROLL, REPEATS,', 'PASSES, REPEATS,')
    text = replace_once(text, 'placement = -1; placement < 8', 'placement = 0; placement < 2')
    text = replace_once(text, 'unsigned good = 1;', 'unsigned good = 1;\n                uint32_t last_result = 0;')
    text = replace_once(text, 'data[1] = 5;', 'data[1] = 13;')
    text = replace_once(text, 'data[1] == 5', 'data[1] == 13')
    text = replace_once(text, 'critical_exit(state);', 'critical_exit(state);\n                    last_result = result;')
    text = replace_once(text, 'passes=%lu ticks=', 'passes=%lu unroll=%u ticks=')
    text = replace_once(text, 'size, PASSES,', 'size, PASSES, p->unroll,')
    text = replace_once(text, 'p->result, good);', '(unsigned)last_result, good);')
    text = text.replace('pipeline.log', 'portq.log').replace('PIPELINE v1', 'PORTQ v1')
    text = text.replace('END PIPELINE', 'END PORTQ').replace('Pipeline timing probe', 'Local port timing probe')
    text = text.replace('Done. Tap to return.', 'Done. Power off or return.')
    generated = OUT / 'portq.c'
    generated.write_text(text)
    subprocess.run(['make', '-s', '-C', str(SOURCE.parent), f'OUT={OUT}',
                    'PROGRAM=portq', f'SOURCE={generated}'], check=True)
    tool = ROOT / 'host-tools/toolchain-c33/work/install/bin/c33-epson-elf-nm'
    symbols = dict((name, int(address, 16)) for address, name in re.findall(
        r'^([0-9a-f]+) [A-Za-z] (\w+)$', subprocess.check_output([str(tool), '-n', str(OUT / 'portq.elf')], text=True), re.M))
    for name, row in variants.items():
        if symbols[name] % 16 or symbols[name + '_end'] - symbols[name] != row['bytes']:
            raise ValueError(f'Unexpected instruction layout or branch relaxation: {name}')
    manifest = {'version': 1, 'probes': variants, 'placements': [['a0', 0], ['a0', 2]],
                'data': ['a0', 'sdram'], 'passes': 20000, 'repeats': 3,
                'cases': len(variants) * 4, 'consumer_checksum': 'r10 XOR r11; input=13, initial r10=5',
                'base_source_sha256': sha(SOURCE), 'source_sha256': sha(generated),
                'app_sha256': sha(OUT / 'portq.app')}
    (OUT / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'{manifest["cases"]} variable-length port cases: {OUT}/portq.app')


if __name__ == '__main__':
    main()
