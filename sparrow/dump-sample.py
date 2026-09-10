#!/usr/bin/env python3
"""Build a bounded sample exclusively from published Wikimedia dump files.

Uses the checksum-verified multistream index and byte ranges of its matching
dated XML dump. No MediaWiki API, SPARQL endpoint or entity-data URL is used.
"""
import argparse
import bz2
import hashlib
import json
from pathlib import Path
import re
import subprocess
import time
import xml.etree.ElementTree as ET
import build as builder

DEMO_SEEDS = {'Q965','Q937','Q243','Q676203','Q419','Q254','Q69163529',
              'Q11696','Q23505','Q9960','Q1124','Q142','Q750'}
DATE = '20260901'
PREFIX = 'wikidatawiki-' + DATE + '-pages-articles-multistream'
BASE = 'https://dumps.wikimedia.org'


def digest(path, algorithm='sha256'):
    h = hashlib.new(algorithm)
    with path.open('rb') as f:
        for block in iter(lambda:f.read(1024*1024), b''):
            h.update(block)
    return h.hexdigest()


def locate(index, wanted, file_size, cache):
    """Scan compressed index without writing its decompressed contents."""
    key=hashlib.sha256('\n'.join(sorted(wanted)).encode()).hexdigest()
    saved=cache/f'locations-{key}.json'
    if saved.exists():
        record=json.loads(saved.read_text())
        return record['found'],record['missing']
    wanted = {q.encode() for q in wanted}
    found, pending = {}, []
    previous = None
    scanned = 0
    with bz2.open(index, 'rb') as f:
        for line in f:
            offset, _, title = line.rstrip(b'\n').split(b':', 2)
            if offset != previous:
                for q in pending:
                    found[q.decode()] = (int(previous), int(offset))
                    wanted.remove(q)
                pending = []
                previous = offset
                if not wanted:
                    break
            if title in wanted:
                pending.append(title)
            scanned += 1
            if scanned % 10000000 == 0:
                print(f'Index: {scanned:,} entries scanned, {len(wanted)} targets remaining', flush=True)
        else:
            for q in pending:
                found[q.decode()] = (int(previous), file_size)
                wanted.remove(q)
    missing=sorted(q.decode() for q in wanted)
    saved.write_text(json.dumps({'found':found,'missing':missing})+'\n')
    return found, missing


def ranges(locations):
    result = []
    for start, end in sorted({tuple(v) for v in locations.values()}):
        if result and start-result[-1][1] <= 256*1024 and end-result[-1][0] <= 8*1024*1024:
            result[-1] = (result[-1][0], end)
        else:
            result.append((start,end))
    return result


def read_blocks(cache, locations, meta, audit):
    url = BASE + meta['url']
    expected = f'/wikidatawiki/{DATE}/{PREFIX}.xml.bz2'
    if meta['url'] != expected:
        raise RuntimeError('Unexpected dump URL; only the pinned official dump is accepted')
    found = {}
    blocks = ranges(locations)
    for i, (start,end) in enumerate(blocks,1):
        if not 0 <= start < end <= meta['size']:
            raise RuntimeError('Invalid multistream index range')
        path = cache/f'{start}-{end}.bz2'
        headers = path.with_suffix('.headers')
        if not path.exists():
            builder.disk_guard(cache,24)
            partial = path.with_suffix('.part')
            print(f'Dump block {i}/{len(blocks)}: {end-start:,} compressed bytes',flush=True)
            time.sleep(1)
            subprocess.run(['curl','--fail','--silent','--show-error','--location',
                '--user-agent','WikiReader-Sparrow/0.1 (published dump reader)',
                '--retry','3','--retry-delay','10','--max-time','120',
                '--range',f'{start}-{end-1}','--max-filesize',str(end-start),
                '--dump-header',str(headers),'--output',str(partial),url],check=True)
            header = headers.read_text().lower()
            if f'content-range: bytes {start}-{end-1}/{meta["size"]}' not in header or partial.stat().st_size != end-start:
                raise RuntimeError('Dump server did not return the exact requested byte range')
            partial.replace(path)
        if path.stat().st_size != end-start:
            raise RuntimeError(f'Incomplete cached dump block: {path}')
        with bz2.open(path,'rb') as f:
            raw = f.read(256*1024*1024+1)
            if len(raw)>256*1024*1024:
                raise RuntimeError('Dump block exceeds the host decompression bound')
        record={'url':url,'start':start,'end_exclusive':end,'sha256':digest(path),
                'validation':'exact HTTP Content-Range and size; bzip2 CRC; local SHA-256'}
        audit.append(record)
        for match in re.finditer(rb'<page>.*?</page>',raw,re.S):
            page=ET.fromstring(match[0])
            q=page.findtext('title')
            if q not in locations:
                continue
            text=page.findtext('revision/text')
            if page.find('redirect') is not None or not text:
                continue
            item=json.loads(text)
            if item.get('id')!=q or item.get('type')!='item':
                continue
            item['lastrevid']=int(page.findtext('revision/id'))
            found[q]=item
    return found


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('output',type=Path)
    p.add_argument('--index',type=Path,required=True)
    p.add_argument('--status',type=Path,required=True,help='official dated dumpstatus.json')
    p.add_argument('--seeds',type=Path,help='additional roots from local Wikipedia, never benchmark answers')
    p.add_argument('--cache',type=Path,default=Path('build/wikidata/multistream-20260901'))
    args=p.parse_args()
    if args.output.exists():p.error('output already exists')
    metadata=json.loads(args.status.read_text())['jobs']['articlesmultistreamdumprecombine']
    if metadata['status']!='done':p.error('official dump job is not complete')
    files=metadata['files']; index_meta=files[PREFIX+'-index.txt.bz2']; dump_meta=files[PREFIX+'.xml.bz2']
    if args.index.stat().st_size!=index_meta['size'] or digest(args.index,'sha1')!=index_meta['sha1']:
        p.error('multistream index size/SHA-1 does not match the official dump manifest')
    roots=set(DEMO_SEEDS)
    if args.seeds:roots.update(json.loads(args.seeds.read_text())['seeds'])
    for q in roots:builder.qid(q)
    args.cache.mkdir(parents=True,exist_ok=True)
    audit=[]
    print(f'Locating {len(roots)} sample roots in the verified official dump index...',flush=True)
    locations,missing=locate(args.index,roots,dump_meta['size'],args.cache)
    data=read_blocks(args.cache,locations,dump_meta,audit)
    needed=set()
    for item in data.values():
        for prop,claims in (item.get('claims') or {}).items():
            if int(prop[1:]) not in builder.PROPERTIES:continue
            for claim in claims:
                if claim.get('rank')=='deprecated':continue
                value=claim.get('mainsnak',{}).get('datavalue',{}).get('value')
                if isinstance(value,dict):
                    if value.get('id','').startswith('Q'):needed.add(value['id'])
                    if '/Q' in value.get('unit',''):needed.add(value['unit'].rsplit('/',1)[-1])
    print(f'Locating {len(needed-set(data))} referenced entities in the same dump...',flush=True)
    locations,absent=locate(args.index,needed-set(data),dump_meta['size'],args.cache)
    leaves=read_blocks(args.cache,locations,dump_meta,audit)
    for item in leaves.values():item['claims']={}
    data.update(leaves)
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text('\n'.join(json.dumps(data[q],ensure_ascii=False) for q in sorted(data))+'\n')
    manifest={'source_kind':'official-wikimedia-multistream-dump','dump_date':DATE,
        'dump_url':BASE+dump_meta['url'],'official_whole_dump_sha1':dump_meta['sha1'],
        'whole_dump_downloaded':False,'index_sha1':index_meta['sha1'],
        'status_sha256':digest(args.status),'selection':str(args.seeds) if args.seeds else 'original demo roots',
        'seed_entities':sorted(roots),'leaf_entities_have_no_claims':True,
        'missing_entities':sorted((roots|needed)-set(data)),
        'revisions':{q:item['lastrevid'] for q,item in sorted(data.items())},
        'compressed_blocks':audit,'output_sha256':digest(args.output),'license':'CC0'}
    args.output.with_suffix('.manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print(f'{len(data)} dump-sourced entities; {len(manifest["missing_entities"])} unavailable; sample only',flush=True)


if __name__=='__main__':main()
