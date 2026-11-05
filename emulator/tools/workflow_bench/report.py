"""Validate raw reader measurements before comparing any timing numbers."""
import re
import statistics


def records(text, prefix, keys):
    result = {}
    for line in text.splitlines():
        if not line.startswith(prefix + ' '):
            continue
        row = dict(re.findall(r'(\w+)=(\S+)', line))
        if prefix == 'ZC':
            title = re.search(r' title=(.*?) index=', line)
            if not title:
                raise ValueError('Missing article title')
            row['title'] = title[1]
        identity = tuple(int(row[k]) for k in keys)
        if identity in result:
            raise ValueError(f'Duplicate {prefix} case {identity}')
        result[identity] = row
    return result


def validate(text, manifest):
    if not manifest['entries'] or manifest['passes'] < 3:
        raise ValueError('Workflow needs entries and at least three repetitions')
    headers = [line for line in text.splitlines() if line.startswith('ZCAL ')]
    total = len(manifest['entries']) * manifest['passes']
    if (len(headers) != 1 or not headers[0].startswith('ZCAL v1 ') or
            len(re.findall(r'^END ZCAL ', text, re.M)) != 1 or
            not re.search(rf'^END ZCAL cases={total} failures=0\s*\Z', text, re.M)):
        raise ValueError('Incomplete or failed workflow run')
    header = dict(re.findall(r'(\w+)=(\S+)', headers[0]))
    expected_header = {'entries': str(len(manifest['entries'])),
                       'passes': str(manifest['passes']), 'warmup': '0',
                       'ticks_per_us': '60', 'scrolls': '4'}
    if any(header.get(k) != v for k, v in expected_header.items()):
        raise ValueError('Workflow header differs from the manifest')
    if any(not re.fullmatch(r'[0-9a-f]{8}', header.get(k, ''))
           for k in ('sdram_ctl', 'refresh', 'clkcntl', 'pll')):
        raise ValueError('Missing workflow clock context')
    articles = records(text, 'ZC', ('pass', 'entry'))
    scrolls = records(text, 'ZCS', ('pass', 'entry', 'step'))
    expected = {(p, e) for p in range(1, manifest['passes'] + 1)
                for e in range(len(manifest['entries']))}
    if (articles.keys() != expected or scrolls.keys() !=
            {(*key, step) for key in expected for step in range(4)}):
        raise ValueError('Workflow case coverage differs')
    if manifest['version'] != 1 or manifest['scroll_moves'] != [104, 104, -104, -104]:
        raise ValueError('Unsupported workflow manifest')
    for key, row in articles.items():
        entry = manifest['entries'][key[1]]
        if (row['ok'] != '1' or row['title'] != entry['title'] or
                entry['archive'].lower() not in row['archive'].lower() or int(row['index']) <= 0):
            raise ValueError(f'Unverified article: {key}')
        for name in ('lookup_ticks', 'retrieve_ticks', 'raw', 'text', 'article'):
            if int(row[name]) <= 0:
                raise ValueError(f'Empty workflow phase: {key}, {name}')
        retrieve, opened, painted, screen = (int(row[name]) for name in
                ('retrieve_ticks', 'open_ticks', 'paint_ticks', 'screen_ticks'))
        if not 0 < retrieve <= opened <= painted <= screen:
            raise ValueError(f'Invalid workflow phase ordering: {key}')
        for phase in ('lookup', 'retrieve'):
            calls, sectors, ticks = (int(row[phase + suffix]) for suffix in
                                      ('_reads', '_sectors', '_read_ticks'))
            if min(calls, sectors, ticks) < 0 or calls > sectors or ticks > int(row[phase + '_ticks']):
                raise ValueError(f'Invalid I/O counters: {key}, {phase}')
        if not re.fullmatch(r'[0-9a-f]{8}', row['frame']) or not 0 <= int(row['images']) <= 16:
            raise ValueError(f'Invalid article output: {key}')
        position = None
        direct_steps = 0
        for step, delta in enumerate(manifest['scroll_moves']):
            scroll = scrolls[(*key, step)]
            before, after = int(scroll['before']), int(scroll['after'])
            if (scroll['ok'] != '1' or int(scroll['ticks']) <= 0 or
                    int(scroll['delta']) != delta or after - before != delta or
                    min(before, after) < 0 or
                    (position is not None and before != position) or
                    scroll['direct'] not in ('0', '1') or
                    not re.fullmatch(r'[0-9a-f]{8}', scroll['frame'])):
                raise ValueError(f'Invalid viewport step: {key}, {step}')
            position = after
            direct_steps += int(scroll['direct'])
        if not direct_steps:
            raise ValueError(f'Viewport steps did not exercise direct scrolling: {key}')
    # Repetitions must execute the same work and produce the same pixels.
    for entry in range(len(manifest['entries'])):
        for p in range(2, manifest['passes'] + 1):
            first, other = articles[(1, entry)], articles[(p, entry)]
            for field in ('archive', 'title', 'index', 'raw', 'text', 'article', 'images', 'frame'):
                if first[field] != other[field]:
                    raise ValueError(f'Workflow output changed between repetitions: {entry}, {field}')
            for step in range(4):
                first, other = scrolls[(1, entry, step)], scrolls[(p, entry, step)]
                for field in ('before', 'after', 'direct', 'frame'):
                    if first[field] != other[field]:
                        raise ValueError(f'Viewport output changed between repetitions: {entry}, {step}, {field}')
    return articles, scrolls


def startup(text):
    if (not text.startswith('ZIMBOOT v1 dma_bits=32\n') or
            text.count('END ZIMBOOT') != 1 or not text.rstrip().endswith('END ZIMBOOT') or
            'dma_disabled_at_start=0 dma_disabled_at_end=0' not in text):
        raise ValueError('Incomplete or unhealthy startup report')
    phases = {}
    for name in ('file', 'indexes', 'ui'):
        lines = [line for line in text.splitlines() if line.startswith(name + ' ')]
        if len(lines) != 1:
            raise ValueError(f'Missing or repeated startup phase: {name}')
        row = {k: int(v) for k, v in re.findall(r'(\w+)=(\d+)', lines[0])}
        if (row['elapsed_us'] <= 0 or row['read_us'] > row['elapsed_us'] or
                any(row[k] for k in ('read_errors', 'timeouts', 'dma_errors'))):
            raise ValueError(f'Unhealthy startup phase: {name}')
        phases[name] = row
    totals = dict(re.findall(r'(\w+)=(\d+)', text.splitlines()[2]))
    opened = int(totals['open_to_keyboard_us'])
    if (abs(opened - sum(row['elapsed_us'] for row in phases.values())) > 3 or
            int(totals['app_to_keyboard_us']) < opened):
        raise ValueError('Inconsistent startup phase totals')
    kernel = [line for line in text.splitlines() if line.startswith('KERNELBOOT ')]
    if len(kernel) < 4:
        raise ValueError('Incomplete kernel startup phases')
    for seq, line in enumerate(kernel):
        row = dict(re.findall(r'(\w+)=(\d+)', line))
        if int(row['seq']) != seq or any(int(row[k]) for k in ('read_errors', 'timeouts', 'dma_errors')):
            raise ValueError('Unhealthy kernel startup phase')
    return {'phases': phases, 'totals': {k: int(v) for k, v in totals.items()},
            'cold_boot_repetitions': 1}


def summarize(text, manifest):
    articles, scrolls = validate(text, manifest)
    rows = []
    for entry, identity in enumerate(manifest['entries']):
        for phase in ('lookup', 'retrieve', 'open', 'paint', 'screen'):
            ticks = [int(articles[(p, entry)][phase + '_ticks'])
                     for p in range(1, manifest['passes'] + 1)]
            rows.append({**identity, 'phase': phase, 'ticks': ticks,
                         'median_us': statistics.median(ticks) / 60})
        for step in range(4):
            ticks = [int(scrolls[(p, entry, step)]['ticks'])
                     for p in range(1, manifest['passes'] + 1)]
            rows.append({**identity, 'phase': f'viewport-{step}', 'ticks': ticks,
                         'median_us': statistics.median(ticks) / 60})
    return rows


def compare(model_text, device_text, manifest):
    """Compare matched reports; caller must first verify binary/image identities."""
    ma, ms = validate(model_text, manifest)
    da, ds = validate(device_text, manifest)
    contexts = [dict(re.findall(r'(\w+)=(\S+)', text.splitlines()[0]))
                for text in (model_text, device_text)]
    for field in ('sdram_ctl', 'clkcntl', 'pll', 'refresh'):
        values = [int(row[field], 16) for row in contexts]
        if field == 'refresh':
            values = [value & 0x01ff0fff for value in values]  # writable bits
        if values[0] != values[1]:
            raise ValueError(f'Workflow clock context differs: {field}')
    for key in da:
        for field in ('archive', 'title', 'index', 'raw', 'text', 'article', 'images',
                      'frame', 'lookup_reads', 'lookup_sectors', 'retrieve_reads', 'retrieve_sectors'):
            if ma[key][field] != da[key][field]:
                raise ValueError(f'Workflow work/output differs: {key}, {field}')
    for key in ds:
        for field in ('before', 'after', 'direct', 'frame'):
            if ms[key][field] != ds[key][field]:
                raise ValueError(f'Viewport work/output differs: {key}, {field}')
    results = []
    for m, d in zip(summarize(model_text, manifest), summarize(device_text, manifest)):
        results.append({'archive': d['archive'], 'title': d['title'], 'phase': d['phase'],
                        'model_us': m['median_us'], 'device_us': d['median_us'],
                        'median_relative_error': m['median_us'] / d['median_us'] - 1,
                        'pass_relative_errors': [mt / dt - 1 for mt, dt in zip(m['ticks'], d['ticks'])]})
    return {'phases': results,
            'all_phase_medians_within_5_percent': all(abs(r['median_relative_error']) <= .05 + 1e-12
                                                     for r in results),
            'full_workflow_qualification': False}
