#!/usr/bin/env python3
"""Golden queries, an independent binary reader, malformed-input and I/O checks.
The fixture is synthetic, not a snapshot or a claim of Wikidata coverage.
"""
import copy
import importlib.util
import json
from pathlib import Path
import random
import re
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('builder', ROOT / 'build.py')
b = importlib.util.module_from_spec(spec)
spec.loader.exec_module(b)


def time(y, m=1, d=1, precision=11):
    return {'time': f'+{y:04}-{m:02}-{d:02}T00:00:00Z', 'precision': precision,
            'calendarmodel': 'http://www.wikidata.org/entity/Q1985727', 'before': 0, 'after': 0, 'timezone': 0}


def snak(value, typ='wikibase-entityid'):
    if typ == 'wikibase-entityid':
        value = {'id': f'Q{value}', 'entity-type': 'item', 'numeric-id': value}
    return {'snaktype': 'value', 'datavalue': {'type': typ, 'value': value}}


def claim(value, typ='wikibase-entityid', rank='normal', qualifiers=None):
    return {'mainsnak': snak(value, typ), 'rank': rank, 'qualifiers': qualifiers or {}}


def fixture():
    labels = {1:'Burkina Faso', 2:'Ouagadougou', 3:'Albert Einstein', 4:'Ulm',
              5:'Machu Picchu', 6:'Peru', 7:'Spanish', 8:'Quechua', 9:'Aymara',
              10:'Eiffel Tower', 11:'France', 12:'euro', 13:'metre', 14:'Wolfgang Amadeus Mozart',
              15:'Fall of the Berlin Wall', 16:'President of the United States',
              17:'George H. W. Bush', 18:'Ronald Reagan', 19:'Bill Clinton',
              20:'Ambiguous One', 21:'Ambiguous Two', 22:'Incomplete Country',
              23:'Unknown Capital', 24:'Historical Place', 25:'Precise Person',
              26:'Uncertain Person', 27:'Many Borders', 28:'Transition Event',
              29:'Café', 30:'Null Country', 31:'Scope Country', 32:'Preferred Country',
              33:'Future Holder', 34:'Easter Egg Capital', 35:'Broken Date',
              36:'Example Novel', 37:'Example Writer', 38:'Example Architect',
              39:'Example University', 40:'Example Comic', 41:'Example Game',
              42:'Year Country', 43:'Approx Office', 44:'Approx Holder',
              45:'Example Grandparent', 46:'First Child', 47:'Second Child',
              48:'First Grandchild', 49:'Shared Grandchild', 50:'Empty Branch',
              51:'Unsafe Grandparent', 52:'Unsafe Child', 53:'Many Children',
              54:'Unknown Children', 55:'Preferred Children', 56:'Other Child',
              57:'Duplicate Capital', 58:'Wide Grandparent', 59:'Wide Child',
              60:'Qualified Unknown Children', 61:'Canonical Place', 62:'Alias Pretender',
              63:'Case Place', 64:'case place', 65:'Unknown Grandparent'}
    items = {q: {'id': f'Q{q}', 'type': 'item', 'labels': {'en': {'value': label}},
                 'sitelinks': {'enwiki': {'title': label}}, 'claims': {}, 'aliases': {}} for q,label in labels.items()}
    def put(q,p,*cs):
        for i,c in enumerate(cs): c['id'] = f'Q{q}$synthetic-P{p}-{i}'
        items[q]['claims'][f'P{p}'] = list(cs)
    def alias(q,*names): items[q]['aliases']['en'] = [{'value': n} for n in names]
    put(1,36,claim(2)); put(3,19,claim(4)); alias(3,'einstien')
    put(5,17,claim(6)); put(6,37,claim(7),claim(8),claim(9)); put(6,36,claim(2))
    put(10,17,claim(11)); put(11,38,claim(12))
    put(10,2048,claim({'amount': '+330', 'unit': 'http://www.wikidata.org/entity/Q13'}, 'quantity'))
    put(14,569,claim(time(1756,1,27),'time')); put(14,570,claim(time(1791,12,5),'time')); alias(14,'mozart')
    put(15,585,claim(time(1989,11,9),'time')); alias(15,'the berlin wall fell')
    alias(16,'us president')
    for q,start,end in [(17,1989,1993),(18,1981,1989),(19,1993,2001)]:
        put(q,39,claim(16,qualifiers={'P580':[snak(time(start,1,20),'time')], 'P582':[snak(time(end,1,20),'time')]}))
    put(33,39,claim(16,qualifiers={'P580':[snak(time(2025,0,0,9),'time')]}))
    alias(20,'collision'); alias(21,'collision'); put(20,36,claim(2)); put(21,36,claim(4))
    put(22,36,claim(999999))
    put(24,36,claim(2,qualifiers={'P580':[snak(time(1900),'time')],'P582':[snak(time(1950),'time')]}))
    for q,birth,death in [(25,time(2000,12,31),time(2020,1,1)),(26,time(1900,0,0,9),time(1950,0,0,9)),(35,time(1900,2,30),time(1950))]:
        put(q,569,claim(birth,'time')); put(q,570,claim(death,'time'))
    put(27,47,*(claim(q) for q in range(1,10)))
    put(28,585,claim(time(1989,1,20),'time'))
    put(29,36,claim(2))
    put(30,36,{'mainsnak': {'snaktype':'novalue'},'rank':'preferred'})
    put(31,36,claim(2,qualifiers={'P518':[snak(4)]}))
    put(32,36,claim(4),claim(2,rank='preferred'),claim(4,rank='deprecated'))
    put(34,36,claim(2))
    put(36,50,claim(37)); put(40,170,claim(37)); put(37,27,claim(6))
    put(10,84,claim(38)); put(38,69,claim(39)); put(41,178,claim(38))
    put(3,20,claim(4)); put(14,86,claim(37))
    put(42,1082,claim({'amount':'+1234','unit':'1'},'quantity',qualifiers={'P585':[snak(time(2020,0,0,9),'time')]}))
    put(42,36,claim(2,qualifiers={'P580':[snak(time(2020,1,1),'time')], 'P582':[snak(time(2020,0,0,9),'time')]}))
    put(44,39,claim(43,qualifiers={'P580':[snak(time(1980,0,0,9),'time')], 'P582':[snak(time(1990,0,0,9),'time')]}))
    put(2,1082,claim({'amount':'+4567','unit':'1'},'quantity'))
    put(2,6,claim(37)); put(37,22,claim(38))
    put(36,127,claim(38)); put(37,1303,claim(39))
    put(37,1477,claim({'text':'Original Name','language':'en'},'monolingualtext'))
    put(38,1477,claim({'text':'Autre nom','language':'fr'},'monolingualtext'))
    put(3,26,claim(14))
    put(45,40,claim(46),claim(47),claim(46),claim(50))
    put(46,40,claim(48),claim(49)); put(47,40,claim(49))
    put(46,22,claim(37)); put(47,22,claim(37)); put(56,22,claim(38))
    for q in (46,47,56): put(q,25,claim(39))
    put(51,40,claim(46),claim(52)); put(52,40,claim(49,qualifiers={'P518':[snak(48)]}))
    put(53,40,*(claim(q) for q in range(1,10)))
    put(54,40,claim(999999))
    put(55,40,claim(46),claim(47,rank='preferred'),claim(48,rank='deprecated'))
    put(57,36,*(claim(2) for _ in range(12)))
    put(58,40,claim(46),claim(59)); put(59,40,*(claim(q) for q in range(1,9)))
    put(60,40,claim(999999,qualifiers={'P518':[snak(48)]}))
    put(61,36,claim(2)); put(62,36,claim(4)); alias(62,'Canonical Place')
    put(63,36,claim(2)); put(64,36,claim(4))
    put(65,40,claim(54))
    # Published XML revision JSON uses [] for some empty PHP maps.
    items[13]['sitelinks'] = []  # units without articles still exist
    items[13]['labels'] = []
    items[13]['claims'] = []
    items[13]['aliases'] = []
    items[1]['claims']['P36'][0]['qualifiers'] = []
    return list(items.values())


class Reference:
    """Read the published binary layout independently of the C implementation."""
    def __init__(self,path):
        self.data = path.read_bytes()
        self.buckets, self.count, self.bp, self.ep = struct.unpack_from('<IIQQ',self.data,24)
    def block(self,table,i):
        off,n = struct.unpack_from('<QI',self.data,table+i*16)
        return self.data[off:off+n]
    def entity(self,i):
        data=self.block(self.ep,i)
        q,ln,tn,n,_ = struct.unpack_from('<IHHHH',data)
        pos=12
        label=data[pos:pos+ln].decode(); pos+=ln+tn
        cs=[]
        for _ in range(n):
            prop,kind,rank,obj,date,start,end,asof,flags,vl,ul,sl,_=struct.unpack_from('<HBBIiiiiHHHHI',data,pos)
            pos+=36
            value=data[pos:pos+vl].decode(); pos+=vl
            unit=data[pos:pos+ul].decode(); pos+=ul
            source=data[pos:pos+sl].decode(); pos+=sl
            cs.append(dict(prop=prop,kind=kind,rank=rank,obj=obj,date=date,start=start,end=end,asof=asof,flags=flags,value=value,unit=unit,source=source))
        assert pos == len(data)
        return label,cs
    def aliases(self):
        out={}
        for bucket in range(self.buckets):
            data=self.block(self.bp,bucket); pos=0
            while pos<len(data):
                id_,n=struct.unpack_from('<IH',data,pos); pos+=6
                key=data[pos:pos+n].decode(); pos+=n
                out[key]=id_
        return out


def main():
    cli=Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix='sparrow-test-') as tmp:
        tmp=Path(tmp); dump=tmp/'fixture.jsonl'; out=tmp/'sparrow.dat'
        dump.write_text('\n'.join(json.dumps(x) for x in fixture())+'\n')
        subprocess.run([sys.executable,str(ROOT/'build.py'),str(dump),str(out),'--snapshot','synthetic-test'],check=True,stdout=subprocess.DEVNULL)
        ref=Reference(out); aliases=ref.aliases()
        queries = [
            ('capital of burkina faso',0,'Ouagadougou'),
            ('WHAT IS THE CAPITAL OF BURKINA_FASO?',0,'Ouagadougou'),
            ('einstien birthplace',0,'Ulm'),
            ('how tall is eiffel tower',0,'330'),
            ('what language do they speak where machu picchu is',0,'3 recorded values'),
            ('what currency do they use where eiffel tower is',0,'euro'),
            ('capital of the country machu picchu is in',0,'Ouagadougou'),
            ('how old was mozart when he died',0,'35 years'),
            ('how old was precise person at death',0,'19 years'),
            ('who was us president when the berlin wall fell',0,'George H. W. Bush'),
            ('who was us president when transition event',2,'Ambiguous'),
            ('capital of collision',2,'Ambiguous'),
            ('capital of missing',1,'No answer'),
            ('capital of incomplete country',3,'No answer'),
            ('capital of null country',3,'No answer'),
            ('capital of scope country',3,'No answer'),
            ('capital of preferred country',0,'Ouagadougou'),
            ('capital of historical place',0,'Ouagadougou'),
            ('how old was uncertain person at death',3,'No answer'),
            ('how old was broken date at death',3,'No answer'),
            ('countries that border many borders',6,'Query limit'),
            ('capital of café',0,'Ouagadougou'),
            ('capital of cafe',1,'No answer'),
            ('capital of burkina faso in 1800',1,'No answer'),
            ('not capital of burkina faso',3,'No answer'),
            ('is tokyo bigger than london',3,'No answer'),
            ('who would win a fight between a bear and a shark',3,'No answer'),
            ('marie curie partner',3,'No answer'),
            ('what language do they speak where machu picchu is not',3,'No answer'),
            ('x'*128,6,'Query limit'),
            ('What are the official languages of Peru?',0,'3 recorded values'),
            ('In which country is Machu Picchu located?',0,'Peru'),
            ('Who wrote the book Example Novel?',0,'Example Writer'),
            ('Who created the comic Example Comic?',0,'Example Writer'),
            ('Who developed the video game Example Game?',0,'Example Architect'),
            ('Where did Albert Einstein die?',0,'Ulm'),
            ('Where did the architect of Eiffel Tower study?',0,'Example University'),
            ('Which country does the creator of Example Comic come from?',0,'Peru'),
            ('Who did not write Example Novel?',3,'No answer'),
            ('Who wrote Example Novel in 1900?',1,'No answer'),
            ('Who composed Example Novel?',1,'No answer'),
            ('In which city did Albert Einstein die?',3,'No answer'),
            ('what is the population of year country',0,'1234'),
            ('capital of year country',0,'Ouagadougou'),
            ('who was approx office when the berlin wall fell',3,'No answer'),
            ('How many people live in the capital of Burkina Faso?',0,'4567'),
            ('Who is the mayor of the capital of Burkina Faso?',0,'Example Writer'),
            ('father of the author of Example Novel',0,'Example Architect'),
            ('father of the author of the capital of Burkina Faso',1,'No answer'),
            ('population of the capital of the country of Machu Picchu',0,'4567'),
            ('population of the capital of the country of the birthplace of Albert Einstein',6,'Query limit'),
            ('population of the capital of historical place',2,'Ambiguous'),
            ('population of the capital of collision',2,'Ambiguous'),
            ('Who is the owner of Example Novel?',0,'Example Architect'),
            ('Which instruments does Example Writer play?',0,'Example University'),
            ('What is the birth name of Example Writer?',0,'Original Name'),
            ('What is the birth name of Example Architect?',3,'No answer'),
            ('Where did the Example Architect study?',0,'Example University'),
            ('Where did the collision study?',2,'Ambiguous'),
            ('Who wrote Example Novel.',0,'Example Writer'),
            ('Who wrote Example Novel!',0,'Example Writer'),
            ('Who did not write Example Novel.',3,'No answer'),
            ('when did the spouse of Albert Einstein die',0,'1791-12-05'),
            ('How many children does Example Grandparent have?',0,'3 recorded children'),
            ('How many children did Many Children have?',0,'9 recorded children'),
            ('How many children does Unknown Children have?',0,'1 recorded children'),
            ('How many children does Qualified Unknown Children have?',3,'No answer'),
            ('How many children does Empty Branch have?',1,'No answer'),
            ('How many children does Preferred Children have?',0,'1 recorded children'),
            ('How many official languages does Peru have?',0,'3 recorded official language'),
            ('How many borders does Many Borders have?',0,'9 recorded borders'),
            ('Did Example Grandparent have children?',0,'Yes'),
            ('Did Empty Branch have children?',1,'No answer'),
            ('Did Unknown Children have children?',0,'Yes'),
            ('Did Qualified Unknown Children have children?',3,'No answer'),
            ('Was Albert Einstein married?',0,'Yes'),
            ('Is First Child a child of Example Grandparent?',0,'Yes'),
            ('Is Aymara a child of Many Children?',0,'Yes'),
            ('Is First Grandchild a child of Example Grandparent?',1,'No answer'),
            ('Was Albert Einstein married to Mozart?',0,'Yes'),
            ('Did Example Architect study at Example University?',0,'Yes'),
            ('Was Albert Einstein born in Ulm?',0,'Yes'),
            ('Do First Child and Second Child have the same parents?',0,'Yes'),
            ('Do First Child and Other Child have the same parents?',0,'No: recorded parents differ'),
            ('Do First Child and Empty Branch have the same parents?',1,'No answer'),
            ('Give me the grandchildren of Example Grandparent.',0,'2 recorded values'),
            ('Grandchildren of Unsafe Grandparent',3,'No answer'),
            ('Grandchildren of Wide Grandparent',6,'Query limit'),
            ('Grandchildren of Many Children',6,'Query limit'),
            ('Grandchildren of Unknown Children',3,'No answer'),
            ('Grandchildren of Unknown Grandparent',3,'No answer'),
            ('In which year was Mozart born?',0,'1756'),
            ('In which year was Uncertain Person born?',3,'No answer'),
            ('capital of Duplicate Capital',0,'Ouagadougou'),
            ('capital of Canonical Place',0,'Ouagadougou'),
            ('capital of Case Place',2,'Ambiguous'),
        ]
        for query,status,answer in queries:
            run=subprocess.run([str(cli),str(out),query],capture_output=True,text=True)
            assert run.returncode == bool(status), (query,run)
            assert f'status={status}' in run.stdout, (query,run.stdout,run.stderr)
            assert run.stdout.splitlines()[0].startswith(answer), (query,run.stdout)
            reads,size=map(int,re.search(r'reads=(\d+) bytes=(\d+)',run.stdout).groups())
            assert reads<=12 and size<=250*1024, (query,reads,size)
            if status: assert ' -> ' not in run.stdout
        # The parser and the device use one plan; diagnostics must preserve
        # hop order and distinguish missing aliases from missing facts.
        plan=subprocess.run([str(cli),'--plan','population of the capital of the country of Machu Picchu'],capture_output=True,text=True,check=True)
        assert json.loads(plan.stdout)['properties']==[17,36,1082]
        run=subprocess.run([str(cli),str(out),'when did the spouse of Albert Einstein die','--json'],capture_output=True,text=True,check=True)
        value=json.loads(run.stdout)['values'][0]
        assert value['kind']==2 and value['date']==17911205
        for question, expected in [('How many children does Many Children have?', ['9']),
                                   ('Give me the grandchildren of Example Grandparent', [48,49]),
                                   ('Was Albert Einstein married?', ['true']),
                                   ('how old was mozart at death', ['35'])]:
            run=subprocess.run([str(cli),str(out),question,'--json'],capture_output=True,text=True,check=True)
            values=json.loads(run.stdout)['values']
            actual=sorted(v['qid'] or v['value'] for v in values)
            assert actual==expected,(question,values)
        run=subprocess.run([str(cli),str(out),'How many children does Many Children have?','--html'],capture_output=True,text=True,check=True)
        assert 'first eight supporting claims' in run.stdout and '9 recorded children' in run.stdout
        run=subprocess.run([str(cli),str(out),'Is Aymara a child of Many Children?','--html'],capture_output=True,text=True,check=True)
        assert '>Aymara</a>' in run.stdout and 'Q53$synthetic-P40-8' in run.stdout
        for question,stage in [('why is the sky blue','wording'),('capital of missing','entity'),
                               ('Who composed Example Novel','claims'),('capital of Burkina Faso','answered')]:
            run=subprocess.run([str(cli),str(out),question,'--json'],capture_output=True,text=True)
            assert json.loads(run.stdout)['stage']==stage,(question,run.stdout)
        # Differential fact decoding across every safe unqualified subject.
        run=subprocess.run([str(cli),str(out),'population of year country'],capture_output=True,text=True,check=True)
        assert 'as of 2020 [' in run.stdout and '2020-00' not in run.stdout
        run=subprocess.run([str(cli),str(out),'population of year country','--html'],capture_output=True,text=True,check=True)
        assert 'as of 2020</li>' in run.stdout and '2020-00' not in run.stdout
        assert b.qualifier_date(time(2020,5,1,10))==20200500
        assert b.qualifier_date(dict(time(2020,0,0,9),before=1))==0
        assert b.qualifier_date(dict(time(2020,0,0,9),calendarmodel='http://www.wikidata.org/entity/Q1985786'))==0
        props={36:'capital',19:'birthplace',17:'country',37:'official language',38:'currency',2048:'height'}
        differential=0
        for name,id_ in aliases.items():
            if id_==b.MISSING: continue
            label,cs=ref.entity(id_)
            for prop,suffix in props.items():
                selected=[c for c in cs if c['prop']==prop]
                if not selected: continue
                best=max(c['rank'] for c in selected)
                selected=[c for c in selected if c['rank']==best]
                if any(c['flags'] for c in selected) or len(selected)>8: continue
                run=subprocess.run([str(cli),str(out),name+' '+suffix],capture_output=True,text=True)
                assert run.returncode==0,(name,suffix,run.stdout)
                for c in selected:
                    assert f"{label} -> {suffix} -> {c['value']}" in run.stdout,run.stdout
                differential+=1
        # Byte reproducibility and compressed line-delimited JSON array input.
        import gzip
        zipped=tmp/'array.json.gz'
        with gzip.open(zipped,'wt') as f:
            f.write('[\n'+',\n'.join(json.dumps(x) for x in fixture())+'\n]\n')
        other=tmp/'other.dat'
        subprocess.run([sys.executable,str(ROOT/'build.py'),str(zipped),str(other),'--snapshot','synthetic-test'],check=True,stdout=subprocess.DEVNULL)
        assert out.read_bytes()==other.read_bytes()
        # Aggregation has separate set, evidence, fan-out and byte bounds.
        # These claims are synthetic stress inputs, never benchmark data.
        bounded_items=fixture()
        def add(q,label,targets,source_padding=0):
            bounded_items.append({'id':f'Q{q}','type':'item','labels':{'en':{'value':label}},
                'sitelinks':{'enwiki':{'title':label}},'claims':{'P40':[
                    dict(claim(target),id=f'Q{q}$'+str(i).zfill(source_padding or 1))
                    for i,target in enumerate(targets)]}})
        add(70,'Set Boundary',range(900000,900256))
        add(71,'Set Overflow',range(900000,900257))
        add(72,'Eight Branches',range(73,81))
        for q in range(73,81): add(q,f'Branch {q}',[48])
        add(81,'Byte Overflow',range(82,90))
        for q in range(82,90): add(q,f'Large Branch {q}',[48]*500,40)
        bounds_dump=tmp/'bounds.jsonl'; bounds_index=tmp/'bounds.dat'
        bounds_dump.write_text('\n'.join(json.dumps(x) for x in bounded_items)+'\n')
        subprocess.run([sys.executable,str(ROOT/'build.py'),str(bounds_dump),str(bounds_index),
                        '--snapshot','synthetic-bounds'],check=True,stdout=subprocess.DEVNULL)
        assert not json.loads(Path(str(bounds_index)+'.json').read_text()).get('oversize_property_groups')
        for question,status,value in [('How many children does Set Boundary have?',0,'256'),
                                      ('How many children does Set Overflow have?',6,None),
                                      ('Grandchildren of Eight Branches',0,48),
                                      ('Grandchildren of Byte Overflow',6,None)]:
            run=subprocess.run([str(cli),str(bounds_index),question,'--json'],capture_output=True,text=True)
            result=json.loads(run.stdout)
            assert result['status']==status,(question,result)
            assert result['reads']<=64 and result['bytes']<=250*1024,result
            if value is not None:
                assert len(result['values'])==1,result
                v=result['values'][0]
                assert (v['qid'] or v['value'])==value,result
            else: assert not result['values'],result
        # A pathological property must abstain as a whole while unrelated
        # facts remain available; original statements remain in the audit.
        huge=fixture()
        huge[0]['claims']['P36']=[dict(claim(2),id='Q1$'+str(i).zfill(120)) for i in range(600)]
        huge[0]['claims']['P19']=[dict(claim(4),id='Q1$birthplace')]
        huge[0]['labels']['en']['value']='界'*250
        huge[0]['sitelinks']['enwiki']['title']='界'*250
        huge[0]['aliases']['en']=[{'value':'large fixture'}]
        oversized=tmp/'oversized.jsonl'; bounded=tmp/'bounded.dat'
        oversized.write_text('\n'.join(json.dumps(x) for x in huge)+'\n')
        subprocess.run([sys.executable,str(ROOT/'build.py'),str(oversized),str(bounded),'--snapshot','synthetic-large'],check=True,stdout=subprocess.DEVNULL)
        manifest=json.loads(Path(str(bounded)+'.json').read_text())
        assert manifest['oversize_property_groups']==1 and manifest['device_claims_omitted_for_size']==600
        assert manifest['labels_shown_as_qid']==manifest['oversize_article_links_omitted']==1
        run=subprocess.run([str(cli),str(bounded),'large fixture birthplace'],capture_output=True,text=True)
        assert run.returncode==0 and run.stdout.startswith('Ulm\n'),run.stdout+run.stderr
        run=subprocess.run([str(cli),str(bounded),'capital of large fixture'],capture_output=True,text=True)
        assert run.returncode==1 and 'status=3' in run.stdout,run.stdout+run.stderr
        with gzip.open(str(bounded)+'.statements.jsonl.gz','rt') as f:
            assert sum(json.loads(line)['property']==36 and json.loads(line)['subject']=='Q1' for line in f)==600
        # Relocate the entity directory and all payloads above 4 GiB in a
        # sparse file. The alias directory stays at its specified offset.
        import zlib
        original=out.read_bytes()
        base=0x100000000
        large=tmp/'large.dat'
        header=bytearray(original[:128])
        struct.pack_into('<Q',header,16,base+len(original))
        struct.pack_into('<Q',header,40,base+ref.ep)
        struct.pack_into('<I',header,80,zlib.crc32(header[:80]))
        with large.open('wb') as f:
            f.truncate(base+len(original)); f.write(header)
            f.seek(base+ref.ep); f.write(original[ref.ep:])
            for table,count,destination in [(ref.bp,ref.buckets,ref.bp),(ref.ep,ref.count,base+ref.ep)]:
                for i in range(count):
                    off,n,crc=struct.unpack_from('<QII',original,table+i*16)
                    f.seek(destination+i*16); f.write(struct.pack('<QII',off+base,n,crc))
        run=subprocess.run([str(cli),str(large),'capital of burkina faso','--json'],capture_output=True,text=True)
        assert run.returncode==0,run.stderr+run.stdout
        assert json.loads(run.stdout)['values'][0]['qid']==2
        corrupt=tmp/'corrupt.dat'
        mutations=[]
        for offset,value in [(0,b'BADMAGIC'),(8,struct.pack('<I',999)),(24,struct.pack('<I',3)),(40,struct.pack('<Q',2**63)),(512,struct.pack('<Q',2**63))]:
            data=bytearray(original); data[offset:offset+len(value)]=value; mutations.append(bytes(data))
        mutations += [original[:n] for n in (0,10,127,512,len(original)-1)]
        rng=random.Random(101)
        for _ in range(100):
            data=bytearray(original)
            for _ in range(5): data[rng.randrange(len(data))]=rng.randrange(256)
            mutations.append(bytes(data))
        for data in mutations:
            corrupt.write_bytes(data)
            run=subprocess.run([str(cli),str(corrupt),'capital of burkina faso'],capture_output=True,text=True)
            assert run.returncode in (0,1,2),(run.returncode,run.stderr)
            assert 'runtime error:' not in run.stderr and 'AddressSanitizer' not in run.stderr,run.stderr
        print(f'{len(queries)} golden queries, {differential} reference comparisons, {len(mutations)} malformed files; deterministic build and offsets above 4 GiB: PASS')


if __name__=='__main__': main()
