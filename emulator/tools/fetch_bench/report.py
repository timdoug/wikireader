"""Require complete external-fetch coverage, layout and native output checks."""
import re

KEYS = ('probe', 'branch', 'code', 'offset', 'data', 'iqb', 'passes')
SHAPES = {'alu8': (8, 0), 'hold8': (8, 1), 'copy0': (0, 2),
          'copy4': (4, 2), 'copy8': (8, 2)}


def expected_cases():
    expected = set()
    for shape, (_, kind) in SHAPES.items():
        for branch in ('normal', 'delayed'):
            for data in (['a0'] if kind == 0 else ['a0', 'sdram'] if kind == 1 else ['sdram']):
                for offset in range(0, 16, 2):
                    for iqb in (0, 1):
                        for passes in (1, 8, 8192):
                            expected.add((shape, branch, 'sdram', str(offset), data, str(iqb), str(passes)))
    for shape, data in [('alu8', 'a0'), ('hold8', 'a0'), ('copy4', 'sdram')]:
        for branch in ('normal', 'delayed'):
            for iqb in (0, 1):
                for passes in (8, 8192):
                    expected.add((shape, branch, 'a0', '0', data, str(iqb), str(passes)))
    assert len(expected) == 600
    return expected


def fields(line):
    pairs = re.findall(r'(\w+)=(\S+)', line)
    if len(dict(pairs)) != len(pairs):
        raise ValueError('Duplicate report field')
    return dict(pairs)


def validate(text, manifest):
    lines = text.splitlines()
    if (not lines or not lines[0].startswith('FETCHQ v1 ') or
            lines[-1] != 'END FETCHQ cases=600 failures=0' or len(lines) != 602):
        raise ValueError('Incomplete or failed external-fetch report')
    if (manifest['version'] != 1 or manifest['cases'] != 600 or manifest['repeats'] != 3 or
            manifest['passes'] != [1, 8, 8192] or manifest['offsets'] != list(range(0, 16, 2)) or
            manifest['iqb'] != [0, 1]):
        raise ValueError('Unsupported external-fetch manifest')
    header = fields(lines[0])
    addresses = manifest['addresses']
    for key, value in [('code', addresses['sdram_code']), ('data', addresses['external']),
                       ('a0', addresses['local_code']), ('wrapper', addresses['measure']),
                       ('stack', addresses['local_stack'])]:
        if header.get(key) != f'{value:08x}':
            raise ValueError(f'Changed probe placement: {key}')
    if header.get('repeats') != '3':
        raise ValueError('Changed repetition count')
    for key in ('sdram_ctl', 'refresh', 'app', 'clkcntl', 'pll'):
        if not re.fullmatch('[0-9a-f]{8}', header.get(key, '')):
            raise ValueError(f'Missing clock/controller context: {key}')
    for key in ('timer0', 'clock0', 'timer5', 'clock5'):
        if not re.fullmatch('[0-9a-f]{4}', header.get(key, '')):
            raise ValueError(f'Missing timer context: {key}')
    rows = {}
    for line in lines[1:-1]:
        if not line.startswith('RESULT '):
            raise ValueError('Unknown external-fetch report line')
        row = fields(line)
        try:
            key = tuple(row[k] for k in KEYS)
            if key in rows or key not in expected_cases():
                raise ValueError('Duplicate or unexpected external-fetch case')
            shape, branch, code, offset, data, iqb, passes = key
            padding, kind = SHAPES[shape]
            symbol = shape + ('_d' if branch == 'delayed' else '_n')
            probe = manifest['probes'][symbol]
            if probe['padding'] != padding or probe['kind'] != kind or probe['loop_offset'] != 16:
                raise ValueError('Changed native instruction shape')
            base = addresses['local_code'] if code == 'a0' else addresses['sdram_code'] + 256
            entry = base + int(offset)
            src = addresses['external'] + 0x1000 if data == 'sdram' else addresses['local_data'] + 4
            dst = addresses['external'] + 0x21000 if data == 'sdram' else addresses['local_data'] + 8
            for field, value in [('entry', entry), ('loop', entry + 16), ('src', src), ('dst', dst)]:
                if row[field] != f'{value:08x}':
                    raise ValueError(f'Changed measured address: {field}')
            ticks = [int(x) for x in row['ticks'].split(',')]
            result = (13 if kind else 5) ^ (padding * int(passes))
            if (row['ok'] != '1' or int(row['result']) != result or
                    int(row['bytes']) != probe['bytes'] or len(ticks) != 3 or min(ticks) <= 0):
                raise ValueError('Failed native result, guard or timing sample')
        except (KeyError, TypeError) as error:
            raise ValueError('Missing external-fetch case context') from error
        rows[key] = row
    if rows.keys() != expected_cases():
        raise ValueError('Incomplete external-fetch coverage')
    return header, rows
