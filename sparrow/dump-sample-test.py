#!/usr/bin/env python3
"""Synthetic multistream fixtures; no network or real knowledge data."""
import bz2
import importlib.util
import json
from pathlib import Path
import tempfile
from xml.sax.saxutils import escape

spec=importlib.util.spec_from_file_location('dump_sample',Path(__file__).with_name('dump-sample.py'))
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)


def page(q,label):
    entity={'type':'item','id':q,'labels':{'en':{'value':label}},'claims':{}}
    return ('<page><title>'+q+'</title><revision><id>123</id><text>'+escape(json.dumps(entity))+'</text></revision></page>').encode()


with tempfile.TemporaryDirectory() as tmp:
    tmp=Path(tmp)
    one=bz2.compress(b'<mediawiki>'+page('Q1','One & Only')+page('Q2','Two'))
    two=bz2.compress(page('Q3','Three')+b'</mediawiki>')
    data=one+two
    index=tmp/'index.bz2'
    index.write_bytes(bz2.compress(f'0:1:Q1\n0:2:Q2\n{len(one)}:3:Q3\n'.encode()))
    locations,missing=m.locate(index,{'Q1','Q3','Q999'},len(data),tmp)
    assert locations=={'Q1':(0,len(one)),'Q3':(len(one),len(data))} and missing==['Q999']
    cached,absent=m.locate(index,{'Q1','Q3','Q999'},len(data),tmp)
    assert m.ranges(cached)==m.ranges(locations)==[(0,len(data))] and absent==missing
    (tmp/f'0-{len(data)}.bz2').write_bytes(data)
    def no_network(*args,**kwargs):
        raise AssertionError('a local fixture must never request the network')
    m.subprocess.run=no_network
    meta={'url':f'/wikidatawiki/{m.DATE}/{m.PREFIX}.xml.bz2','size':len(data)}
    audit=[]
    entities=m.read_blocks(tmp,cached,meta,audit)
    assert set(entities)=={'Q1','Q3'}
    assert entities['Q1']['labels']['en']['value']=='One & Only'
    assert entities['Q3']['lastrevid']==123
    assert audit[0]['start']==0 and audit[0]['end_exclusive']==len(data)
    try:
        m.read_blocks(tmp,cached,dict(meta,url='/w/api.php'),[])
    except RuntimeError:
        pass
    else:
        raise AssertionError('only the official dated dump path is accepted')
print('PASS: compressed index lookup/cache, complete stream ranges, XML entity extraction, revisions and dump-only URL constraint')
