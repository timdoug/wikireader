#!/usr/bin/env python3
"""Compare generated C programs after FLASH -> Grifo -> init -> app boot."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def compile_program(command, log):
    with log.open('w') as stream:
        subprocess.run(command, check=True, stdout=stream, stderr=subprocess.STDOUT)


def run_seed(seed, args, fat):
    stage = Path(tempfile.mkdtemp(prefix=f't{seed}-', dir=args.work))
    source = stage / 'test.c'
    source.write_bytes(args.source.read_bytes() if args.source else
                       subprocess.check_output(['python3', str(HERE / 'gen.py'), str(seed)]))
    compile_source = args.source if args.source else source
    host, obj, app = stage / 'host', stage / 'test.o', stage / 'test.app'
    flags = shlex.split(os.environ.get('DT_OPTS', '-O2'))
    reference = None
    if not args.target_only:
        compile_program(['cc', '-DDT_HOST', '-w', *flags, '-I', str(HERE / 'runtime'),
                         str(compile_source), '-o', str(host)], stage / 'host-compile.log')
        reference = subprocess.check_output([str(host)], timeout=30).decode()
        (stage / 'host.out').write_text(reference)
        assert reference and all(re.fullmatch(r'[0-9a-f]{8}', s) for s in reference.splitlines())
    cc = str(args.toolchain / 'c33-epson-elf-gcc')
    output_flags = ['-DDT_FILE_OUTPUT'] if args.file_output else []
    compile_program([cc, '-mc33pe', '-w', *flags, *shlex.split(args.target_flags), '-Dmain=dt_main',
                     *output_flags, '-I', str(HERE / 'runtime'), '-c', str(compile_source), '-o', str(obj)],
                    stage / 'target-compile.log')
    wrapper = stage / 'app.o'
    compile_program([cc, '-mc33pe', '-O2', '-mlong-calls', '-fno-builtin', *output_flags,
                     '-I' + str(ROOT / 'samo-lib/grifo/include'),
                     '-I' + str(ROOT / 'samo-lib/include'),
                     '-I' + str(ROOT / 'samo-lib/mini-libc/include'),
                     '-c', str(HERE / 'runtime/app.c'), '-o', str(wrapper)],
                    stage / 'wrapper-compile.log')
    # GCC 3.3.2's linker cannot parse the current application's OVERLAY
    # sections. Use the production linker with the selected compiler's
    # own arithmetic helpers; the compiler producing the tested code stays
    # independent of the emulator and of the newer compiler.
    libgcc = subprocess.check_output([cc, '-mc33pe', '-print-libgcc-file-name'], text=True).strip()
    ld = Path(os.environ.get('DT_LD', ROOT / 'host-tools/toolchain-c33/work/install/bin/c33-epson-elf-ld'))
    compile_program([str(ld), '-T', str(ROOT / 'samo-lib/grifo/lds/application.lds'),
                     str(wrapper), str(obj), *map(str, args.extra_object),
                     str(ROOT / 'samo-lib/grifo/lib/libgrifo.a'),
                     str(ROOT / 'samo-lib/mini-libc/lib/libc.a'), libgcc,
                     *map(str, args.extra_library), '-o', str(app)],
                    stage / 'link.log')
    files = {'kernel.elf': (ROOT / 'samo-lib/grifo/grifo.elf').read_bytes(),
             'init.app': (ROOT / 'samo-lib/grifo/applications/init/init.app').read_bytes(),
             'test.app': app.read_bytes(), 'test.ico': bytes(512),
             'init.ini': b'test.ico : test.app\n'}
    card, flash = stage / 'card.img', fat.build_flash(stage)
    fat.make_image(card, files, 64)
    command = [str(args.wremu), '-P', '-e', str(flash), '-c', str(card), '-n', str(args.limit)]
    identity = dict(seed=seed, flags=flags, command=command,
                    target_flags=shlex.split(args.target_flags),
                    target_only=args.target_only,
                    output_transport='Grifo filesystem' if args.file_output else 'UART',
                    extra_inputs={str(p): sha(p) for p in args.extra_object + args.extra_library},
                    emulator_sha256=sha(args.wremu), compiler_sha256=sha(Path(cc)),
                    linker_sha256=sha(ld), libgcc_sha256=sha(Path(libgcc)),
                    source_sha256=sha(source), flash_sha256=sha(flash),
                    setting=os.environ.get('WREMU_MODEL', ''),
                    input_sha256={name: hashlib.sha256(data).hexdigest() for name, data in files.items()})
    (stage / 'inputs.json').write_text(json.dumps(identity, indent=2) + '\n')
    with (stage / 'boot.log').open('w') as log:
        result = subprocess.run(command, cwd=stage, stdout=log, stderr=subprocess.STDOUT, timeout=120)
    text = (stage / 'boot.log').read_text(errors='replace')
    blocks = re.findall(r'^DT_BEGIN\r?\n(.*?)^DT_END\r?$', text, re.M | re.S)
    output = blocks[0].replace('\r', '') if len(blocks) == 1 else ''
    if args.file_output:
        output = (fat.read_file(card, 'difftest.out') or b'').decode('ascii', errors='replace')
    (stage / 'emulator.out').write_text(output)
    checks = {'exit status': result.returncode == 0,
              'complete production boot': bool(re.search(
                  r'load: kernel\.elf.*?Grifo starting.*?init starting.*?DT_BEGIN', text, re.S)),
              'one complete application': text.count('DT_BEGIN') == text.count('DT_END') == 1,
              'nonempty hexadecimal output': bool(output) and all(
                  re.fullmatch(r'[0-9a-f]{8}', s) for s in output.splitlines()),
              'application powered off': 'stop reason: powered off' in text,
              'no guest faults': not any(s in text for s in ('DT_FAILED', 'C33 exception', 'Panic:')),
              'unchanged emulator': sha(args.wremu) == identity['emulator_sha256']}
    if reference is not None:
        checks['native output matches'] = output == reference
    report = dict(**identity, checks=checks, complete=all(checks.values()),
                  values=len(output.splitlines()), returncode=result.returncode,
                  log_sha256=sha(stage / 'boot.log'))
    report['output_sha256'] = hashlib.sha256(output.encode()).hexdigest()
    (stage / 'results.json').write_text(json.dumps(report, indent=2) + '\n')
    if not report['complete']:
        raise RuntimeError(f'{[k for k, v in checks.items() if not v]}: {stage}')
    if args.output:
        args.output.write_text(output)
    print(f'seed {seed}: ok ({report["values"]} values, FLASH/Grifo/init/app)', flush=True)


def coverage(paths):
    counts, runs = {}, 0
    for path in paths:
        for report in Path(path).glob('t*/results.json'):
            record = json.loads(report.read_text())
            log = report.parent / 'boot.log'
            if not record.get('complete') or sha(log) != record['log_sha256']:
                continue
            for opcode, count in re.findall(r'^OP (\S+)\s+(\d+)', log.read_text(errors='replace'), re.M):
                counts[opcode] = counts.get(opcode, 0) + int(count)
            runs += 1
    if not runs:
        raise SystemExit('No verified Grifo-path differential runs found')
    print(f'# {runs} verified runs; counts include FLASH/Grifo/init and application')
    for opcode in sorted(counts):
        print(opcode, counts[opcode])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('first', type=int, nargs='?', default=1)
    parser.add_argument('last', type=int, nargs='?')
    parser.add_argument('--coverage', nargs='+', metavar='DIRECTORY')
    parser.add_argument('--source', type=Path, help='Use an existing C test instead of generating one')
    parser.add_argument('--extra-object', type=Path, action='append', default=[])
    parser.add_argument('--extra-library', type=Path, action='append', default=[])
    parser.add_argument('--target-flags', default='')
    parser.add_argument('--target-only', action='store_true', help='Caller checks results; still verify boot and termination')
    parser.add_argument('--file-output', action='store_true', help='Write large result streams through the real Grifo filesystem')
    parser.add_argument('--output', type=Path, help='Save qualified application output for a single test')
    parser.add_argument('--toolchain', type=Path, default=os.environ.get(
        'DT_TC', ROOT / 'host-tools/toolchain-install/bin'))
    parser.add_argument('--wremu', type=Path, default=os.environ.get('DT_WREMU', ROOT / 'emulator/wremu'))
    parser.add_argument('--work', type=Path, default=os.environ.get('DT_WORK', '/tmp/dt'))
    parser.add_argument('--limit', type=int, default=int(os.environ.get('DT_NINSN', '400000000')))
    args = parser.parse_args()
    if args.coverage:
        coverage(args.coverage)
        return
    args.last = args.first if args.last is None else args.last
    if args.first < 0 or args.last < args.first or args.limit <= 0:
        parser.error('Expected a nonnegative increasing seed range and positive instruction limit')
    if args.output and args.first != args.last:
        parser.error('--output requires a single test')
    args.toolchain, args.wremu, args.work = args.toolchain.resolve(), args.wremu.resolve(), args.work.resolve()
    args.work.mkdir(parents=True, exist_ok=True)
    if args.source:
        args.source = args.source.resolve()
    args.extra_object = [p.resolve() for p in args.extra_object]
    args.extra_library = [p.resolve() for p in args.extra_library]
    spec = importlib.util.spec_from_file_location('fat', ROOT / 'emulator/tools/mem_dma_bench/run.py')
    fat = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fat)
    failed = []
    for seed in range(args.first, args.last + 1):
        try:
            run_seed(seed, args, fat)
        except (AssertionError, OSError, RuntimeError, subprocess.SubprocessError) as error:
            failed.append(seed)
            print(f'seed {seed}: FAILED: {error}', flush=True)
    print(f'{args.last - args.first + 1 - len(failed)} passed, {len(failed)} failed')
    if failed:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
