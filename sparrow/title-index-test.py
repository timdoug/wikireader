#!/usr/bin/env python3
"""Check title-only upgrading and the frozen post-build evaluation workflow."""
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from tests import fixture, Reference

ROOT = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('title_index',ROOT/'title-index.py')
t = importlib.util.module_from_spec(spec); spec.loader.exec_module(t)


def main():
    with tempfile.TemporaryDirectory(prefix='sparrow-titles-') as tmp:
        tmp = Path(tmp); dump = tmp/'fixture.jsonl'; canonical = tmp/'canonical.dat'
        dump.write_text('\n'.join(json.dumps(x) for x in fixture())+'\n')
        subprocess.run([sys.executable,str(ROOT/'build.py'),str(dump),str(canonical),
                        '--snapshot','synthetic-titles'],check=True,stdout=subprocess.DEVNULL)
        original = canonical.read_bytes(); ref = Reference(canonical)
        # Restore the pre-title-policy ambiguity of this real fixture collision.
        old = bytearray(original)
        found = False
        for i in range(ref.buckets):
            off,n = struct.unpack_from('<QI',old,ref.bp+i*16)
            pos = off
            while pos < off+n:
                _,ln = struct.unpack_from('<IH',old,pos)
                if old[pos+6:pos+6+ln] == b'canonical place':
                    struct.pack_into('<I',old,pos,t.MISSING); found = True
                pos += 6+ln
            struct.pack_into('<I',old,ref.bp+i*16+12,zlib.crc32(old[off:off+n]))
        assert found
        source = tmp/'source'; source.mkdir(); index = source/'sparrow.dat'; index.write_bytes(old)
        report = json.loads(Path(str(canonical)+'.json').read_text()); report['sha256'] = t.sha(index)
        Path(str(index)+'.json').write_text(json.dumps(report))
        upgraded = tmp/'upgraded.dat'; result = t.upgrade(index,upgraded)
        assert upgraded.read_bytes() == original
        assert index.read_bytes() == old and result['aliases_disambiguated_by_title'] == 1
        assert Reference(upgraded).aliases()['collision'] == t.MISSING
        assert Reference(upgraded).aliases()['case place'] == t.MISSING
        for name in ('Capital_of_X',' Café\tA  ','_THE_PLACE_', 'ß'):
            from tests import b
            assert t.normalize(name) == b.normalize(name)
        try: t.upgrade(index,upgraded)
        except ValueError: pass
        else: raise AssertionError('existing output was overwritten')
        # Complete a tiny synthetic source job, then run every evaluation arm.
        frozen = source/'frozen'; frozen.mkdir()
        shutil.copy2(ROOT/'build/sparrow',frozen/'sparrow')
        questions = {'questions':[{'id':'synthetic', 'question':[{'language':'en','string':'capital of Canonical Place'}],
            'answers':[{'head':{'vars':['x']},'results':{'bindings':[{'x':{'value':'http://www.wikidata.org/entity/Q2'}}]}}]}]}
        for split in ('train','test'): (frozen/f'{split}.json').write_text(json.dumps(questions))
        (source/'job.json').write_text(json.dumps({'state':'complete'}))
        evaluation = tmp/'evaluation'
        subprocess.run([sys.executable,str(ROOT/'evaluate-build.py'),str(source),str(evaluation)],
                       check=True,stdout=subprocess.DEVNULL)
        job = json.loads((evaluation/'job.json').read_text())
        assert job['state'] == 'complete' and len(job['results']) == 6
        for split in ('train','test'):
            assert job['results'][split+'-before']['abstained'] == 1
            assert job['results'][split+'-operators']['abstained'] == 1
            assert job['results'][split+'-titles']['correct'] == 1
        # Even a file with a recomputed full-file SHA must pass block CRCs.
        broken = bytearray(old); off = struct.unpack_from('<Q',old,ref.ep)[0]; broken[off+12] ^= 1
        index.write_bytes(broken); report['sha256'] = t.sha(index)
        Path(str(index)+'.json').write_text(json.dumps(report))
        try: t.upgrade(index,tmp/'broken.dat')
        except ValueError as error: assert 'checksum' in str(error)
        else: raise AssertionError('damaged entity accepted')
        assert not (tmp/'broken.dat').exists()
    print('PASS: title precedence preserves every claim byte, collisions abstain, CRCs validate; frozen post-build evaluation')


if __name__ == '__main__': main()
