#!/usr/bin/env python3
"""Build controlled external-fetch probes and verify the emitted instructions."""
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / 'build/wr128/fetchq'
SOURCE = Path(__file__).with_name('fetch.c')
SHAPES = {'alu8': (8, 0), 'hold8': (8, 1), 'copy0': (0, 2),
          'copy4': (4, 2), 'copy8': (8, 2)}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    command = ['make', '-s', '-C', str(ROOT / 'emulator/tools/pipeline_bench'),
               f'OUT={OUT}', 'PROGRAM=fetchq', f'SOURCE={SOURCE}']
    subprocess.run(command, check=True)
    shutil.copy2(SOURCE, OUT / 'fetch.c')
    tool = ROOT / 'host-tools/toolchain-c33/work/install/bin/c33-epson-elf-'
    symbols = dict((name, int(address, 16)) for address, name in re.findall(
        r'^([0-9a-f]+) [A-Za-z] (\w+)$',
        subprocess.check_output([str(tool) + 'nm', '-n', str(OUT / 'fetchq.elf')], text=True), re.M))
    disasm = subprocess.check_output([str(tool) + 'objdump', '-d', '-z', str(OUT / 'fetchq.elf')], text=True)
    (OUT / 'disassembly.txt').write_text(disasm)
    words = {int(addr, 16): int(hi + lo, 16) for addr, lo, hi in
             re.findall(r'^\s*([0-9a-f]+):\s+([0-9a-f]{2}) ([0-9a-f]{2})\s', disasm, re.M)}
    probes = {}
    for name, (padding, kind) in SHAPES.items():
        for branch, suffix in [('normal', 'n'), ('delayed', 'd')]:
            symbol = f'{name}_{suffix}'
            start, end = symbols[symbol], symbols[symbol + '_end']
            load = (0x2eaa, 0x247a, 0x257a)[kind]
            store = (0x2eaa, 0x348a, 0x358a)[kind]
            tail = [0x6416, 0x6806, 0x1a00 | (-(padding + 4) & 255)] if suffix == 'n' else [
                0x6416, 0x1b00 | (-(padding + 3) & 255), 0]
            expected = [0x6c5a, 0x6c0b, 0x6c1c] + [0] * 5
            expected += [load] + [0x22cb] * padding + [store] + tail + [0x2ea4, 0x3ab4, 0x0640]
            assert start % 16 == 0 and end - start == len(expected) * 2
            actual = [words[a] for a in range(start, end, 2)]
            assert actual == expected, (symbol, actual, expected)
            probes[symbol] = dict(shape=name, branch=branch, padding=padding, kind=kind,
                                  bytes=end-start, loop_offset=16,
                                  instructions=[f'{word:04x}' for word in actual])
    wrapper = disasm.split('<measure>:\n', 1)[1].split('\n\n', 1)[0]
    assert wrapper.count('call.d  %r4') == 1
    stack_writes = list(re.finditer(r'ld\.w    %sp,%r(\d+)\b', wrapper))
    assert len(stack_writes) == 2
    saved_sp = stack_writes[-1][1]
    assert f'ld.w    %r{saved_sp},%sp' in wrapper[:stack_writes[0].start()]
    interval = wrapper[stack_writes[0].end():stack_writes[1].start()]
    assert not re.search(r'\b(?:push|pop)|\[%(?:sp|r15)\]', interval)
    assert not re.search(r'\b(?:call|xcall)\s+(?!%r4)', interval)
    for name in ('measure', 'local_code', 'local_data', 'local_stack', 'work'):
        assert 0xc00 <= symbols[name] < 0x2000, (name, symbols[name])
    assert symbols['sdram_code'] % 1024 == 0 and symbols['external'] % 0x400000 == 0
    assert symbols['sdram_code'] >> 22 != symbols['external'] >> 22
    manifest = dict(version=1, probes=probes, repeats=3, passes=[1, 8, 8192],
                    offsets=list(range(0, 16, 2)), iqb=[0, 1], cases=600,
                    source_sha256=sha(SOURCE), app_sha256=sha(OUT / 'fetchq.app'),
                    icon_sha256=sha(OUT / 'fetchq.ico'),
                    disassembly_sha256=sha(OUT / 'disassembly.txt'),
                    addresses={name: symbols[name] for name in
                               ('measure', 'local_code', 'local_data', 'local_stack', 'work', 'sdram_code', 'external')},
                    build_command=command,
                    preparation='A0 wrapper sets IQBEN, rewrites one identical halfword in each of eight code lines, then reads back the final word before sampling the timer; no assumption that IQBEN toggling flushes buffers',
                    timed_wrapper='Owned A0 code, parameters and stack; direct high/low/high timer reads; no syscall or external stack access inside interval')
    (OUT / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'600 external-fetch cases and exact template/wrapper checks: {OUT / "fetchq.app"}')


if __name__ == '__main__':
    main()
